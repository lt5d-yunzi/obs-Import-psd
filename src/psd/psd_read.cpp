// SPDX-License-Identifier: GPL-2.0-or-later
//
// PSD reader. See the representation contract at the top of psd.h.
//
// Hardening rules (all enforced here):
//  * every size field is validated against the bytes that are actually left
//    before it is used to allocate or seek;
//  * channel data is decoded inside a reader confined to the layer-info block,
//    so a bogus channel length can never desynchronise the record stream;
//  * RLE row counts are 16-bit by definition, so one channel's packed size is
//    at most rows * 64 KiB and is additionally capped by max_pixels;
//  * every loop is bounded by a size computed from validated data;
//  * buffers are raw arrays (see psd_common.h) and no container is resized
//    while a pointer into it is live.
#include "psd.h"
#include "impl/psd_common.h"

#include <algorithm>
#include <cstdio>
#include <cstring>

namespace psd {

namespace detail {

uint8_t blend_code_from_key(const char *k)
{
    if (!std::memcmp(k, "mult", 4)) return 'M';
    if (!std::memcmp(k, "scrn", 4)) return 'S';
    if (!std::memcmp(k, "over", 4)) return 'O';
    if (!std::memcmp(k, "diss", 4)) return 'D';
    return 'N';
}

const char *blend_key_from_code(uint8_t code)
{
    switch (code) {
    case 'M': return "mult";
    case 'S': return "scrn";
    case 'O': return "over";
    case 'D': return "diss";
    default: return "norm";
    }
}

namespace {

void append_utf8(std::string &s, uint32_t cp)
{
    if (cp < 0x80) {
        s.push_back(char(cp));
    } else if (cp < 0x800) {
        s.push_back(char(0xC0 | (cp >> 6)));
        s.push_back(char(0x80 | (cp & 0x3F)));
    } else if (cp < 0x10000) {
        s.push_back(char(0xE0 | (cp >> 12)));
        s.push_back(char(0x80 | ((cp >> 6) & 0x3F)));
        s.push_back(char(0x80 | (cp & 0x3F)));
    } else {
        s.push_back(char(0xF0 | (cp >> 18)));
        s.push_back(char(0x80 | ((cp >> 12) & 0x3F)));
        s.push_back(char(0x80 | ((cp >> 6) & 0x3F)));
        s.push_back(char(0x80 | (cp & 0x3F)));
    }
}

} // namespace

void utf16_to_utf8(const uint16_t *in, size_t n, std::string &out)
{
    for (size_t i = 0; i < n; ++i) {
        uint32_t cp = in[i];
        if (cp >= 0xD800 && cp <= 0xDBFF && i + 1 < n && in[i + 1] >= 0xDC00 && in[i + 1] <= 0xDFFF) {
            cp = 0x10000u + ((cp - 0xD800u) << 10) + (uint32_t(in[i + 1]) - 0xDC00u);
            ++i;
        } else if (cp >= 0xD800 && cp <= 0xDFFF) {
            cp = 0xFFFD; // lone surrogate
        }
        append_utf8(out, cp);
    }
}

void utf8_to_utf16(const std::string &s, std::vector<uint16_t> &out)
{
    const auto *p = reinterpret_cast<const unsigned char *>(s.data());
    const size_t n = s.size();
    size_t i = 0;
    while (i < n) {
        const unsigned char c = p[i];
        uint32_t cp = 0xFFFD;
        size_t step = 1;
        if (c < 0x80) {
            cp = c;
        } else if ((c & 0xE0) == 0xC0) {
            if (i + 1 < n && (p[i + 1] & 0xC0) == 0x80) {
                const uint32_t v = ((uint32_t(c) & 0x1Fu) << 6) | (uint32_t(p[i + 1]) & 0x3Fu);
                if (v >= 0x80) {
                    cp = v;
                    step = 2;
                }
            }
        } else if ((c & 0xF0) == 0xE0) {
            if (i + 2 < n && (p[i + 1] & 0xC0) == 0x80 && (p[i + 2] & 0xC0) == 0x80) {
                const uint32_t v = ((uint32_t(c) & 0x0Fu) << 12) | ((uint32_t(p[i + 1]) & 0x3Fu) << 6) |
                                   (uint32_t(p[i + 2]) & 0x3Fu);
                if (v >= 0x800 && !(v >= 0xD800 && v <= 0xDFFF)) {
                    cp = v;
                    step = 3;
                }
            }
        } else if ((c & 0xF8) == 0xF0) {
            if (i + 3 < n && (p[i + 1] & 0xC0) == 0x80 && (p[i + 2] & 0xC0) == 0x80 && (p[i + 3] & 0xC0) == 0x80) {
                const uint32_t v = ((uint32_t(c) & 0x07u) << 18) | ((uint32_t(p[i + 1]) & 0x3Fu) << 12) |
                                   ((uint32_t(p[i + 2]) & 0x3Fu) << 6) | (uint32_t(p[i + 3]) & 0x3Fu);
                if (v >= 0x10000 && v <= 0x10FFFF) {
                    cp = v;
                    step = 4;
                }
            }
        }
        i += step;
        if (cp >= 0x10000) {
            cp -= 0x10000;
            out.push_back(uint16_t(0xD800 + (cp >> 10)));
            out.push_back(uint16_t(0xDC00 + (cp & 0x3FF)));
        } else {
            out.push_back(uint16_t(cp));
        }
    }
}

} // namespace detail

namespace {

// A plane of 8-bit samples in a raw array.
using Plane = std::unique_ptr<uint8_t[]>;

Plane alloc_plane(size_t n)
{
    if (n == 0)
        return nullptr;
    uint8_t *p = new (std::nothrow) uint8_t[n];
    if (p != nullptr)
        std::memset(p, 0, n);
    return Plane(p);
}

// Decodes `h` PackBits rows of `w` samples each from `packed` into `out`.
// Rows are independent: whatever the stream claims, a row can never write
// past its own w bytes, and the next row starts at the next declared offset.
void decode_rle_rows(const uint8_t *packed, size_t packed_len, const uint32_t *row_counts, int64_t w, int64_t h,
                     uint8_t *out)
{
    size_t i = 0;
    for (int64_t y = 0; y < h; ++y) {
        size_t row_len = size_t(row_counts[size_t(y)]);
        if (row_len > packed_len - i)
            row_len = packed_len - i;
        const size_t end = i + row_len;
        const size_t row_base = size_t(uint64_t(y) * uint64_t(w));
        const size_t row_limit = row_base + size_t(w);
        size_t produced = row_base;
        while (i < end && produced < row_limit) {
            const uint8_t code = packed[i];
            ++i;
            if (code < 128) {
                size_t cnt = size_t(code) + 1u;
                if (cnt > end - i)
                    cnt = end - i;
                if (cnt > row_limit - produced)
                    cnt = row_limit - produced;
                if (cnt == 0)
                    break;
                std::memcpy(out + produced, packed + i, cnt);
                produced += cnt;
                i += cnt;
            } else if (code != 128) {
                if (i >= end)
                    break;
                size_t cnt = size_t(257u - uint32_t(code));
                if (cnt > row_limit - produced)
                    cnt = row_limit - produced;
                std::memset(out + produced, packed[i], cnt);
                produced += cnt;
                ++i;
            }
        }
        i = end;
    }
}

// Upper bound on the packed (RLE) size of one w*h channel we accept. PackBits
// worst case is < 2w per row and every 16-bit row count caps a row at 65535
// bytes; the result is additionally bounded by max_pixels of slack.
uint64_t channel_packed_cap(int64_t w, int64_t h, uint64_t max_pixels)
{
    const uint64_t area = uint64_t(w) * uint64_t(h);
    uint64_t cap = uint64_t(h) * (uint64_t(w) * 2u + 64u);
    const uint64_t hard = (area > 0x7FFFFFFFFFFFFFFFull) ? area : area * 2u + 0x10000u;
    if (cap > hard)
        cap = hard;
    if (cap < area)
        cap = area;
    if (max_pixels < 0xFFFFFFFFFFFFFFFFull - area && cap > area + max_pixels)
        cap = area + max_pixels;
    return cap;
}

enum class PlaneResult { Ok, Error };

// Reads one 8-bit channel plane of w*h samples from `r` (which is confined to
// the layer-info block) and never reads past the block or `declared` bytes.
PlaneResult read_channel_plane(detail::Reader &r, size_t declared, int64_t w, int64_t h, uint64_t max_pixels,
                               Plane &out, std::string &err)
{
    const uint64_t n = uint64_t(w) * uint64_t(h);
    uint16_t compression = 0;
    if (!detail::rd_u16(r, compression)) {
        err = "truncated channel compression tag";
        return PlaneResult::Error;
    }
    if (compression == 2 || compression == 3) {
        err = "ZIP-compressed channel data is not supported";
        return PlaneResult::Error;
    }
    if (compression != 0 && compression != 1) {
        err = "unsupported channel compression";
        return PlaneResult::Error;
    }
    if (compression == 0) {
        if (uint64_t(r.remaining()) < n) {
            err = "truncated raw channel";
            return PlaneResult::Error;
        }
        const uint8_t *p = r.ptr(size_t(n));
        if (p == nullptr) {
            err = "truncated raw channel";
            return PlaneResult::Error;
        }
        out = alloc_plane(size_t(n));
        if (out == nullptr) {
            err = "out of memory";
            return PlaneResult::Error;
        }
        std::memcpy(out.get(), p, size_t(n));
        return PlaneResult::Ok;
    }
    // RLE: h 16-bit row byte counts, then the concatenated rows.
    if (uint64_t(h) > uint64_t(r.remaining()) / 2u) {
        err = "truncated RLE row counts";
        return PlaneResult::Error;
    }
    if (declared < uint64_t(h) * 2u) {
        err = "RLE channel shorter than its row table";
        return PlaneResult::Error;
    }
    const uint8_t *counts = r.ptr(size_t(h) * 2u);
    if (counts == nullptr) {
        err = "truncated RLE row counts";
        return PlaneResult::Error;
    }
    uint64_t total = 0;
    for (int64_t y = 0; y < h; ++y)
        total += (uint64_t(counts[size_t(y) * 2u]) << 8) | uint64_t(counts[size_t(y) * 2u + 1]);
    if (total > uint64_t(r.remaining())) {
        err = "truncated RLE data";
        return PlaneResult::Error;
    }
    if (total > channel_packed_cap(w, h, max_pixels)) {
        err = "implausible RLE channel length";
        return PlaneResult::Error;
    }
    const uint8_t *packed = r.ptr(size_t(total));
    if (packed == nullptr) {
        err = "truncated RLE data";
        return PlaneResult::Error;
    }
    out = alloc_plane(size_t(n));
    if (out == nullptr) {
        err = "out of memory";
        return PlaneResult::Error;
    }
    std::unique_ptr<uint32_t[]> rc(new (std::nothrow) uint32_t[size_t(h)]);
    if (rc == nullptr) {
        err = "out of memory";
        return PlaneResult::Error;
    }
    for (int64_t y = 0; y < h; ++y)
        rc[size_t(y)] = (uint32_t(counts[size_t(y) * 2u]) << 8) | uint32_t(counts[size_t(y) * 2u + 1]);
    decode_rle_rows(packed, size_t(total), rc.get(), w, h, out.get());
    return PlaneResult::Ok;
}

// Converts loaded planes (a null plane means "missing channel") to
// straight-alpha RGBA8.
void planes_to_rgba(Plane *planes, int r_idx, int g_idx, int b_idx, int a_idx, size_t count, std::vector<uint8_t> &out)
{
    out.assign(count * 4, uint8_t(0));
    if (out.empty())
        return;
    uint8_t *o = out.data();
    for (size_t i = 0; i < count; ++i) {
        const uint8_t R0 = (r_idx >= 0 && planes[r_idx]) ? planes[r_idx][i] : uint8_t(0);
        const uint8_t G0 = (g_idx >= 0 && planes[g_idx]) ? planes[g_idx][i] : R0;
        const uint8_t B0 = (b_idx >= 0 && planes[b_idx]) ? planes[b_idx][i] : R0;
        const uint8_t A = (a_idx >= 0 && planes[a_idx]) ? planes[a_idx][i] : uint8_t(255);
        o[i * 4 + 0] = detail::unpremul(R0, A);
        o[i * 4 + 1] = detail::unpremul(G0, A);
        o[i * 4 + 2] = detail::unpremul(B0, A);
        o[i * 4 + 3] = A;
    }
}

// Reads a Pascal string padded to a multiple of 4 bytes.
bool read_pascal(detail::Reader &r, std::string &out)
{
    uint8_t len = 0;
    if (!detail::rd_u8(r, len))
        return false;
    const uint8_t *p = r.ptr(len);
    if (p == nullptr)
        return false;
    out.assign(reinterpret_cast<const char *>(p), len);
    const size_t pad = (4u - ((size_t(len) + 1u) % 4u)) % 4u;
    return pad == 0 || r.skip_n(pad) == pad;
}

// Maps channel ids of one layer record to plane slots.
bool map_layer_channels(const int16_t *ids, size_t nchan, int mode, int &r, int &g, int &b, int &a)
{
    r = g = b = a = -1;
    for (size_t i = 0; i < nchan; ++i) {
        const int16_t id = ids[i];
        if (id == -1) {
            if (a < 0)
                a = int(i);
            continue;
        }
        if (id < 0)
            continue; // mask (-2) / real user mask (-3): not colour
        if (mode == 3) {
            if (id == 0 && r < 0)
                r = int(i);
            else if (id == 1 && g < 0)
                g = int(i);
            else if (id == 2 && b < 0)
                b = int(i);
        } else if (r < 0) {
            r = int(i); // grayscale & co.: first colour channel is the luminance
        }
    }
    if (r < 0 && a >= 0)
        return true; // pure alpha layer: colour is zero by construction
    if (r < 0)
        return false;
    if (g < 0)
        g = r;
    if (b < 0)
        b = r;
    return true;
}

bool map_composite_channels(int mode, int channels, int &r, int &g, int &b, int &a)
{
    r = g = b = a = -1;
    if (mode == 3) {
        if (channels < 3)
            return false;
        r = 0;
        g = 1;
        b = 2;
        if (channels >= 4)
            a = 3;
        return true;
    }
    if (mode == 4)
        return false; // CMYK: no conversion available
    if (channels < 1)
        return false;
    r = g = b = 0;
    if (channels >= 2)
        a = channels - 1;
    return true;
}

bool is_divider_key(const char *key) { return !std::memcmp(key, "lsct", 4) || !std::memcmp(key, "lsdk", 4); }

// Parses the layer-info block (count + records + channel data) out of
// buf[0..buf_len). On failure returns false with `err` set and nothing usable
// in `out`; the caller then relies on the composite.
bool parse_layers(const uint8_t *buf, size_t buf_len, int color_mode, uint64_t max_pixels, std::vector<Layer> &out,
                  std::string &err)
{
    out.clear();
    // The layer count is an INT16 at offset 0 of the layer-info block (PSD spec).
    // A negative value only means "the first alpha channel is merged", the real
    // number of records is its absolute value.
    if (buf_len < 2) {
        err = "truncated layer count";
        return false;
    }
    const int16_t count16 = int16_t(uint16_t((uint16_t(buf[0]) << 8) | uint16_t(buf[1])));
    if (count16 == 0)
        return true;
    const int64_t count = count16 < 0 ? -int64_t(count16) : int64_t(count16);
    if (uint64_t(count) > uint64_t(buf_len) / 16u) {
        err = "implausible layer count";
        return false;
    }
    out.reserve(size_t(count < 4096 ? count : 4096));
    detail::Reader li(buf, buf_len);
    li.skip_n(2); // count, already consumed
    for (int64_t idx = 0; idx < count; ++idx) {
        const std::string where = " (layer record " + std::to_string(idx) + ")";
        Layer L;
        int32_t top = 0, left = 0, bottom = 0, right = 0;
        uint16_t nch = 0;
        if (!detail::rd_i32(li, top) || !detail::rd_i32(li, left) || !detail::rd_i32(li, bottom) ||
            !detail::rd_i32(li, right) || !detail::rd_u16(li, nch)) {
            err = "truncated layer record header" + where;
            return false;
        }
        const int64_t lw = int64_t(right) - int64_t(left);
        const int64_t lh = int64_t(bottom) - int64_t(top);
        const bool has_pixels = lw > 0 && lh > 0;
        if (has_pixels && uint64_t(lw) * uint64_t(lh) > max_pixels) {
            err = "layer bounds too large" + where;
            return false;
        }
        if (nch > 56) {
            err = "too many channels" + where;
            return false;
        }
        std::vector<int16_t> ids(nch);
        std::vector<uint32_t> lengths(nch);
        for (uint16_t c = 0; c < nch; ++c) {
            int16_t id = 0;
            uint32_t len = 0;
            if (!detail::rd_i16(li, id) || !detail::rd_u32(li, len)) {
                err = "truncated channel info" + where;
                return false;
            }
            ids[c] = id;
            lengths[c] = len;
        }
        const uint8_t *sig = li.ptr(4);
        const uint8_t *blend = li.ptr(4);
        if (sig == nullptr || blend == nullptr) {
            err = "truncated layer blend fields" + where;
            return false;
        }
        char keybuf[5] = {0, 0, 0, 0, 0};
        std::memcpy(keybuf, blend, 4);
        uint8_t opacity = 0, clipping = 0, flags = 0, filler = 0;
        if (!detail::rd_u8(li, opacity) || !detail::rd_u8(li, clipping) || !detail::rd_u8(li, flags) ||
            !detail::rd_u8(li, filler)) {
            err = "truncated layer flags" + where;
            return false;
        }
        uint32_t extra_len = 0;
        if (!detail::rd_u32(li, extra_len) || uint64_t(extra_len) > uint64_t(li.remaining())) {
            err = "bad extra data length" + where;
            return false;
        }
        L.blend_key.assign(keybuf, 4);
        L.blend_mode = detail::blend_code_from_key(keybuf);
        L.opacity = float(opacity);
        L.clipped = (clipping & 1) != 0;
        L.visible = (flags & 2) == 0;
        L.left = left;
        L.top = top;
        L.right = right;
        L.bottom = bottom;

        // --- extra data block (confined reader) ---------------------------
        const uint8_t *extra = li.cur();
        bool is_divider = false;
        uint32_t divider = 0;
        {
            detail::Reader ex(extra, size_t(extra_len));
            uint32_t mask_len = 0;
            if (detail::rd_u32(ex, mask_len) && uint64_t(mask_len) <= uint64_t(ex.remaining()) &&
                (mask_len == 0 || ex.skip_n(mask_len) == mask_len)) {
                uint32_t br_len = 0;
                if (detail::rd_u32(ex, br_len) && uint64_t(br_len) <= uint64_t(ex.remaining()) &&
                    (br_len == 0 || ex.skip_n(br_len) == br_len) && !read_pascal(ex, L.name))
                    L.name.clear();
            }
            size_t scan = ex.pos();
            while (scan + 12 <= size_t(extra_len)) {
                if (std::memcmp(extra + scan, "8BIM", 4) != 0)
                    break; // unknown/misaligned block: stop, keep what we have
                char bkey[5] = {0, 0, 0, 0, 0};
                std::memcpy(bkey, extra + scan + 4, 4);
                detail::Reader bh(extra + scan + 8, size_t(extra_len) - scan - 8);
                uint32_t blen = 0;
                if (!detail::rd_u32(bh, blen))
                    break;
                const size_t bstart = scan + 12;
                if (uint64_t(blen) > uint64_t(extra_len) - bstart)
                    break;
                if (!std::memcmp(bkey, "luni", 4) && blen >= 4) {
                    detail::Reader ur(extra + bstart, size_t(blen));
                    uint32_t nchars = 0;
                    if (detail::rd_u32(ur, nchars) && uint64_t(nchars) * 2u <= uint64_t(ur.remaining())) {
                        std::vector<uint16_t> units;
                        units.reserve(size_t(std::min<uint32_t>(nchars, 65536u)));
                        bool full = true;
                        for (uint32_t k = 0; k < nchars; ++k) {
                            uint16_t u = 0;
                            if (!detail::rd_u16(ur, u)) {
                                full = false;
                                break;
                            }
                            units.push_back(u);
                        }
                        if (full) {
                            std::string uname;
                            detail::utf16_to_utf8(units.data(), units.size(), uname);
                            L.name = std::move(uname);
                        }
                    }
                } else if (is_divider_key(bkey) && blen >= 4) {
                    detail::Reader dr(extra + bstart, size_t(blen));
                    uint32_t type = 0;
                    if (detail::rd_u32(dr, type) && type <= 3) {
                        is_divider = true;
                        divider = type;
                    }
                }
                uint64_t adv = 12u + uint64_t(blen);
                if (bkey[3] == 'm')
                    adv += (8u - (uint64_t(blen) % 8u)) % 8u;
                else
                    adv += (4u - (uint64_t(blen) % 4u)) % 4u;
                if (adv > uint64_t(extra_len) - scan)
                    break;
                scan += size_t(adv);
            }
        }
        if (extra_len != 0 && li.skip_n(size_t(extra_len)) != size_t(extra_len)) {
            err = "truncated extra data" + where;
            return false;
        }

        if (is_divider) {
            // Section divider: group structure only, never pixels. psd.h
            // defines the mapping: "<" (2) is a group marker, "</" (1) names it.
            if (divider == 2) {
                if (!out.empty() && out.back().blend_key == "group") {
                    if (!L.name.empty())
                        out.back().name = L.name; // empty group: both carry the name
                } else {
                    Layer G;
                    G.name = L.name;
                    G.blend_key = "group";
                    G.blend_mode = 'N';
                    G.visible = false;
                    G.opacity = 0.0f;
                    out.push_back(std::move(G));
                }
            } else if (divider == 1) {
                if (!out.empty() && out.back().blend_key == "group" && L.name != out.back().name)
                    out.back().name = L.name;
            }
            continue;
        }

        // --- channel data -------------------------------------------------
        std::unique_ptr<Plane[]> planes(new (std::nothrow) Plane[ids.size()]);
        for (size_t c = 0; c < ids.size(); ++c) {
            if (uint64_t(lengths[c]) > uint64_t(li.remaining())) {
                err = "truncated channel data" + where;
                return false;
            }
            const size_t block_end = li.pos() + size_t(lengths[c]);
            if (has_pixels) {
                const PlaneResult pr = read_channel_plane(li, lengths[c], lw, lh, max_pixels, planes[ids.size() > 0 ? c : 0], err);
                if (pr == PlaneResult::Error) {
                    err += where;
                    return false;
                }
            }
            // A channel body never spills past its declared length.
            if (li.pos() > block_end)
                li.set_pos(block_end);
            if (li.pos() < block_end && li.skip_n(block_end - li.pos()) != block_end - li.pos()) {
                err = "truncated channel data" + where;
                return false;
            }
        }
        if (has_pixels) {
            int lri = -1, lgi = -1, lbi = -1, lai = -1;
            if (map_layer_channels(ids.data(), ids.size(), color_mode, lri, lgi, lbi, lai))
                planes_to_rgba(planes.get(), lri, lgi, lbi, lai, size_t(lw) * size_t(lh), L.rgba);
            else
                L.rgba.assign(size_t(lw) * size_t(lh) * 4, uint8_t(0));
        } else {
            L.rgba.clear();
        }
        out.push_back(std::move(L));
    }
    return true;
}

// Reads the flattened composite starting at `start`.
void read_composite(const uint8_t *data, size_t size, size_t start, int64_t w, int64_t h, int channels, int ri,
                    int gi, int bi, int ai, uint64_t max_pixels, std::vector<uint8_t> &out, std::string &err)
{
    out.clear();
    if (uint64_t(w) * uint64_t(h) > max_pixels) {
        err = "document too large";
        return;
    }
    detail::Reader cr(data, size);
    if (start > size) {
        err = "no composite image data";
        return;
    }
    cr.set_pos(start);
    uint16_t compression = 0;
    if (!detail::rd_u16(cr, compression)) {
        err = "truncated composite data";
        return;
    }
    if (compression != 0 && compression != 1) {
        err = "unsupported composite compression";
        return;
    }
    const int maxc = std::max(std::max(ri, gi), std::max(bi, ai)) + 1;
    std::unique_ptr<Plane[]> planes(new (std::nothrow) Plane[static_cast<size_t>(maxc)]);
    if (!planes) { err = "out of memory"; return; }
    const uint64_t plane_bytes = uint64_t(w) * uint64_t(h);
    for (int c = 0; c < channels; ++c) {
        const bool want = c == ri || c == gi || c == bi || c == ai;
        if (compression == 0) {
            if (uint64_t(cr.remaining()) < plane_bytes) {
                err = "truncated raw composite channel";
                return;
            }
            const uint8_t *p = cr.ptr(size_t(plane_bytes));
            if (want) {
                planes[size_t(c)] = alloc_plane(size_t(plane_bytes));
                if (planes[size_t(c)] == nullptr) {
                    err = "out of memory";
                    return;
                }
                for (size_t q = 0; q < size_t(plane_bytes); ++q) planes[size_t(c)][q] = p[q];
            }
        } else {
            Plane p;
            const PlaneResult pr = read_channel_plane(cr, size_t(cr.remaining()), w, h, max_pixels, p, err);
            if (pr == PlaneResult::Error) {
                return;
            }
            if (want)
                planes[size_t(c)] = std::move(p);
        }
    }
    planes_to_rgba(planes.get(), ri, gi, bi, ai, size_t(plane_bytes), out);
    bool nz = false; for (size_t i = 0; i < out.size(); ++i) if (out[i] != 0) { nz = true; break; }
}

// Core parser. `data` must point at a stable buffer that is not mutated while
// the call runs (read_memory() passes its own private copy).
bool parse_document(const uint8_t *data, size_t size, Document &out, std::string &error, const ReadOptions &opts)
{
    if (size < 26 || std::memcmp(data, "8BPS", 4) != 0) {
        error = "not a PSD file (bad signature)";
        return false;
    }

    // --- fixed header ------------------------------------------------------
    detail::Reader hr(data, size);
    hr.skip_n(4);
    uint16_t version = 0, channels = 0, depth = 0, cmode = 0;
    uint32_t height = 0, width = 0;
    bool hdr_ok = detail::rd_u16(hr, version);
    if (hdr_ok)
        hdr_ok = hr.skip_n(6) == 6;
    if (hdr_ok)
        hdr_ok = detail::rd_u16(hr, channels);
    if (hdr_ok)
        hdr_ok = detail::rd_u32(hr, height);
    if (hdr_ok)
        hdr_ok = detail::rd_u32(hr, width);
    if (hdr_ok)
        hdr_ok = detail::rd_u16(hr, depth);
    if (hdr_ok)
        hdr_ok = detail::rd_u16(hr, cmode);
    if (!hdr_ok) {
        error = "truncated PSD header";
        return false;
    }
    if (version == 2) {
        error = "PSB (large document) files are not supported";
        return false;
    }
    if (version != 1) {
        error = "unsupported PSD version";
        return false;
    }
    if (width == 0 || height == 0) {
        error = "document has zero size";
        return false;
    }
    if (depth != 8) {
        error = "only 8-bit PSD documents are supported";
        return false;
    }
    if (cmode > 9 || channels == 0 || channels > 56) {
        error = "unsupported colour mode or channel count";
        return false;
    }
    if (uint64_t(width) * uint64_t(height) > opts.max_pixels) {
        error = "document too large for this reader";
        return false;
    }
    out.width = int32_t(width);
    out.height = int32_t(height);
    out.depth = depth;
    out.channels = channels;
    const int color_mode = int(cmode);
    const size_t nchan = size_t(channels);
    const int64_t W = int64_t(width), H = int64_t(height);

    // --- colour mode data + image resources (both skipped) -----------------
    for (int sec = 0; sec < 2; ++sec) {
        const char *what = sec == 0 ? "truncated colour mode data" : "truncated image resources";
        uint32_t len = 0;
        if (!detail::rd_u32(hr, len)) {
            error = what;
            return false;
        }
        if (uint64_t(len) > uint64_t(hr.remaining())) {
            error = what;
            return false;
        }
        if (hr.skip_n(size_t(len)) != size_t(len)) {
            error = what;
            return false;
        }
    }

    // --- layer & mask information ------------------------------------------
    uint32_t layer_len = 0;
    if (!detail::rd_u32(hr, layer_len)) {
        error = "truncated layer section length";
        return false;
    }
    if (uint64_t(layer_len) > uint64_t(hr.remaining())) {
        error = "truncated layer and mask information";
        return false;
    }
    const size_t layer_start = hr.pos();
    const size_t layer_end = layer_start + size_t(layer_len);

    bool layers_ok = true;
    std::string layer_error;
    // Composite data starts right after the *layer & mask information* block,
    // which is layer_start + layer_len. (The layer-info sub-length only bounds
    // the layer records themselves, not the whole block.)
    size_t layout_end = layer_end; // where the composite image data starts
    // Be tolerant of writers that under-declare the layer&mask length: if the
    // declared end does not start with a legal compression tag, look for the
    // first plausible composite start at/after layer_start.
    auto valid_comp = [&](size_t at) {
        if (at + 2 > size)
            return false;
        const unsigned c = (unsigned(size > at ? 0 : 0));
        (void)c;
        const unsigned tag = (unsigned(data[at]) << 8) | unsigned(data[at + 1]);
        return tag == 0 || tag == 1;
    };
    if (!valid_comp(layout_end))
        for (size_t probe = layer_start; probe + 2 <= size; ++probe)
            if (valid_comp(probe)) {
                layout_end = probe;
                break;
            }
    if (layer_len >= 4) {
        uint32_t li_len = 0;
        detail::Reader lh(data, size);
        lh.set_pos(layer_start);
        const bool have_len = detail::rd_u32(lh, li_len);
        if (have_len && uint64_t(li_len) <= uint64_t(layer_len - 4)) {
            layout_end = layer_start + 4 + size_t(li_len);
        } else {
            layers_ok = false;
            layer_error = "bad layer info length";
        }
    } else if (layer_len > 0) {
        layers_ok = false;
        layer_error = "bad layer and mask section";
    }

    out.layers.clear();
    if (layers_ok && opts.read_layers && layout_end > layer_start + 4) {
        const size_t li_start = layer_start + 4;
        const size_t li_len = layout_end - li_start;
        // parse_layers gets its own exact-length copy of the block, copied
        // through a translation-unit-opaque routine.
        detail::HeapBuffer block;
        if (!block.reset(li_len)) {
            layer_error = "out of memory";
        } else {
            detail::opaque_copy_bytes(block.data(), data + li_start, li_len);
                    (unsigned)data[li_start], (unsigned)data[li_start+1], (unsigned)data[li_start+2], (unsigned)data[li_start+3]);
            if (!parse_layers(block.data(), li_len, color_mode, opts.max_pixels, out.layers, layer_error))
                out.layers.clear();
        }
    }

    // --- composite ---------------------------------------------------------
    std::string composite_error;
    if (opts.read_composite) {
        int ri = 0, gi = 1, bi = 2, ai = -1;
        const bool mode_ok = map_composite_channels(color_mode, int(nchan), ri, gi, bi, ai);
        if (!mode_ok)
            composite_error = "colour mode cannot be converted to RGB";
        else if (layout_end < layer_start || layout_end >= size)
            composite_error = "no composite image data";
        else {
            // The composite image data stores colour channels only (alpha is not
            // written there), so decode the first (max(ri,gi,bi,ai)+1) planes
            // and ignore any extra ones the header claims.
            const int comp_channels = std::max(std::max(ri, gi), std::max(bi, ai)) + 1;
            read_composite(data, size, layout_end, W, H, comp_channels, ri, gi, bi, ai, opts.max_pixels, out.composite,
                           composite_error);
        }
        if (!out.has_composite())
            out.composite.clear();
    }

    if (out.layers.empty() && !out.has_composite()) {
        error = !layer_error.empty() ? layer_error : composite_error;
        if (error.empty())
            error = "no usable image data found in PSD";
        else if (!layer_error.empty() && !composite_error.empty())
            error += "; " + composite_error;
        return false;
    }
    error.clear();
    return true;
}

} // namespace

bool read_memory(const uint8_t *data, size_t size, Document &out, std::string &error, const ReadOptions &opts)
{
    error.clear();
    out = Document{};
    if (data == nullptr || size == 0) {
        error = "empty file";
        return false;
    }
    // Private copy: the caller's buffer storage may not be stable for the
    // duration of the call (see the compiler note in psd_common.h).
    std::unique_ptr<uint8_t[]> copy(new (std::nothrow) uint8_t[size]);
    if (copy == nullptr) {
        error = "out of memory";
        return false;
    }
    std::memcpy(copy.get(), data, size);
    return parse_document(copy.get(), size, out, error, opts);
}

bool read_file(const std::string &path, Document &out, std::string &error, const ReadOptions &opts)
{
    error.clear();
    out = Document{};
    FILE *f = std::fopen(path.c_str(), "rb");
    if (f == nullptr) {
        error = "cannot open file: " + path;
        return false;
    }
    // Read everything into a private buffer whose storage is provided by an
    // opaque allocator routine (see psd_buf.cpp).
    std::unique_ptr<uint8_t[]> payload;
    size_t size = 0;
    size_t cap = 0;
    bool read_error = false;
    uint8_t chunk[65536];
    size_t got = 0;
    while ((got = std::fread(chunk, 1, sizeof chunk, f)) > 0) {
        if (size + got > cap) {
            size_t ncap = cap ? cap * 2 : 262144;
            while (ncap < size + got)
                ncap *= 2;
            uint8_t *nb = detail::bytebuf_new(ncap);
            if (nb == nullptr) {
                read_error = true;
                break;
            }
            for (size_t i = 0; i < size; ++i)
                nb[i] = payload[i];
            payload.reset(nb);
            cap = ncap;
        }
        for (size_t i = 0; i < got; ++i)
            payload[size + i] = chunk[i];
        size += got;
    }
    const bool ferr = std::ferror(f) != 0;
    std::fclose(f);
    if (read_error || ferr) {
        error = "failed to read whole file";
        return false;
    }
    if (size == 0) {
        error = "empty file";
        return false;
    }
    return read_memory(payload.get(), size, out, error, opts);
}

} // namespace psd
