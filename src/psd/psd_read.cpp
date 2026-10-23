// SPDX-License-Identifier: GPL-2.0-or-later
//
// PSD reader. See the representation contract at the top of psd.h and the
// writer/reader mapping documented at the top of psd_write.cpp.
//
// Hardening rules (all enforced here):
//  * every size field is validated against the bytes that are actually left
//    before it is used to allocate or seek;
//  * channel data is decoded inside a reader confined to the layer-info block,
//    so a bogus channel length can never desynchronise the record stream;
//  * RLE row counts are 16-bit by definition, so one channel's packed size is
//    at most rows * 64 KiB and is additionally capped by max_pixels;
//  * every loop is bounded by a size computed from validated data;
//  * buffers are raw arrays / opaque sinks (see psd_common.h) and no container
//    is resized while a pointer into it is live;
//  * malformed input always ends in `false` + an error message, never in a
//    crash and never in an exception escaping to the caller.
#include "psd.h"

#include "impl/psd_common.h"
#include "psd_descriptor.h"
#include "psd_text.h"

#include "miniz.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <new>

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
            // 128 is a no-op in PackBits.
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

// Reads one 8-bit channel plane of w*h samples from `r` and never reads past
// the end of `r`. `declared` is the channel's declared byte length (a hint:
// some writers include the 2-byte compression tag in it, some do not).
// Inflates a raw-deflate channel body (PSD compression 2/3) into `out`.
// The compressed bytes are the rest of the channel body as declared by the
// record, so everything the caller left in `r` is consumed.
PlaneResult inflate_plane(detail::Reader &r, uint64_t expected, Plane &out, std::string &err)
{
    const size_t avail = r.remaining();
    if (expected > (1ull << 30)) {
        err = "channel too large";
        return PlaneResult::Error;
    }
    const uint8_t *src = r.ptr(avail);
    if (src == nullptr) {
        err = "truncated ZIP channel";
        return PlaneResult::Error;
    }
    out = alloc_plane(size_t(expected));
    if (expected != 0 && out == nullptr) {
        err = "out of memory";
        return PlaneResult::Error;
    }
    if (expected == 0)
        return PlaneResult::Ok;
    mz_ulong dst_len = mz_ulong(expected);
    const int rc = mz_uncompress(out.get(), &dst_len, src, mz_ulong(avail));
    if (rc != MZ_OK && rc != MZ_BUF_ERROR) {
        err = "ZIP channel cannot be inflated";
        return PlaneResult::Error;
    }
    if (uint64_t(dst_len) < expected)
        std::memset(out.get() + dst_len, 0, size_t(expected) - size_t(dst_len));
    return PlaneResult::Ok;
}

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
            return inflate_plane(r, n, out, err);
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
        if (n != 0 && out == nullptr) {
            err = "out of memory";
            return PlaneResult::Error;
        }
        if (n != 0)
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
    if (n != 0 && out == nullptr) {
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

// Converts loaded planes (a null plane means "missing channel") to straight-
// alpha RGBA8. `planes` has max(r,g,b,a)+1 slots; only those are read.
Plane planes_to_rgba(Plane *planes, int r_idx, int g_idx, int b_idx, int a_idx, size_t count)
{
    Plane out = alloc_plane(count * 4);
    if (out == nullptr || count == 0)
        return out;
    uint8_t *o = out.get();
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
    return out;
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

// Decodes one RLE channel plane (compression tag, h row counts, rows) from a
// standalone buffer into a freshly allocated w*h plane.
bool decode_plane_standalone(const uint8_t *body, size_t body_len, int64_t w, int64_t h, Plane &out,
                             std::string &err)
{
    detail::Reader r(body, body_len);
    const PlaneResult pr = read_channel_plane(r, body_len, w, h, 1ull << 28, out, err);
    return pr == PlaneResult::Ok;
}

// Decodes one RLE plane body when the row-count table was already consumed
// (PSD's flattened composite stores one table for all planes).
PlaneResult read_rle_rows_nocounts(detail::Reader &r, int64_t w, int64_t h, const std::vector<uint32_t> &counts,
                                   Plane &out, std::string &err)
{
    const uint64_t n = uint64_t(w) * uint64_t(h);
    uint64_t total = 0;
    for (size_t i = 0; i < counts.size(); ++i)
        total += counts[i];
    if (total > uint64_t(r.remaining())) {
        err = "truncated RLE rows";
        return PlaneResult::Error;
    }
    const uint8_t *packed = r.ptr(size_t(total));
    if (packed == nullptr) {
        err = "truncated RLE rows";
        return PlaneResult::Error;
    }
    out = alloc_plane(size_t(n));
    if (n != 0 && out == nullptr) {
        err = "out of memory";
        return PlaneResult::Error;
    }
    size_t off = 0, produced = 0;
    for (int64_t y = 0; y < h && size_t(y) < counts.size(); ++y) {
        std::vector<uint8_t> row;
        rle_decode(packed + off, counts[size_t(y)], size_t(w), row);
        off += counts[size_t(y)];
        const size_t take = row.size() < size_t(w) ? row.size() : size_t(w);
        if (n != 0)
            std::memcpy(out.get() + produced, row.data(), take);
        produced += size_t(w);
    }
    return PlaneResult::Ok;
}

// Maps channel ids of one layer record to plane slots. Returns false when the
// record has no usable colour channel (the pixels then stay zero).
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

// Maps composite colour channels to plane slots.
bool map_composite_channels(int mode, int channels, int &r, int &g, int &b, int &a)
{
    r = g = b = a = -1;
    if (mode == 3) {
        if (channels < 3)
            return false;
        // The composite of an RGB document holds the three colour planes and
        // never an alpha plane (Photoshop does not write one), independent of
        // the header channel count.
        r = 0;
        g = 1;
        b = 2;
        return true;
    }
    if (mode == 4)
        return false; // CMYK: no conversion available
    if (mode != 1 && mode != 2) // Grayscale / Indexed only
        return false;
    if (channels < 1)
        return false;
    // Grayscale / indexed composites hold a single colour plane.
    r = g = b = 0;
    return true;
}

bool is_divider_key(const char *key) { return !std::memcmp(key, "lsct", 4) || !std::memcmp(key, "lsdk", 4); }

// A group opened by a "<" divider; filled in when the matching "</" is seen.
struct OpenGroup {
    size_t order = 0;   // index of the placeholder in `out`
    size_t first_child = 0; // index of the first member record
    std::string name;
    bool visible = true;
};

// Last-resort recovery: scan forward from the reader's current position for the
// next byte offset that starts a plausible layer record, then parse from there.
// Returns true when it managed to continue parsing to the end of the block.

// Core parser. `data` points at a private, stable copy of the file.
uint64_t rd_be(const uint8_t *p, size_t n)
{
    uint64_t v = 0;
    for (size_t i = 0; i < n; ++i)
        v = (v << 8) | uint64_t(p[i]);
    return v;
}

// Parses the layer-info block (count + records + channel data) out of
// buf[0..buf_len). On failure returns false with `err` set and nothing usable
// in `out`; the caller then relies on the composite.
bool parse_layers(const uint8_t *buf, size_t buf_len, int color_mode, uint64_t max_pixels, int64_t doc_w,
                  int64_t doc_h, std::vector<Layer> &out, std::string &err)
{
    out.clear();
    // The caller passes the layer-info block starting right after its 4-byte
    // length field, so the block starts with the signed 16-bit record count
    // (PSD spec). A negative value only means "the first alpha channel is
    // merged"; the real number of records is the absolute value.
    if (buf_len < 2) {
        err = "truncated layer count";
        return false;
    }
    const uint16_t count_raw = uint16_t((uint16_t(buf[0]) << 8) | uint16_t(buf[1]));
    const int32_t count32 = (count_raw < 0x8000u) ? int32_t(count_raw) : int32_t(count_raw) - 0x10000;
    if (count32 == 0)
        return true;
    const int64_t count = count32 < 0 ? -int64_t(count32) : int64_t(count32);
    // Each record is at least 16 (rect) + 2 (channels) + 8 (blend) + 12 (flags +
    // extra length) + 4 (extra minimum) + 2 (channel length) = 44 bytes, so a
    // larger count can only come from a corrupt file.
    if (uint64_t(count) > uint64_t(buf_len) / 32u) {
        err = "implausible layer count";
        return false;
    }
    out.reserve(size_t(std::min<int64_t>(count, 4096)));
    detail::Reader li(buf, buf_len);
    li.skip_n(2); // 16-bit count

    // Open groups, innermost last. A group is opened by a "<" divider and
    // closed by the matching "</" divider; nesting is supported on read (the
    // placeholder model in psd.h confines written groups to one level).
    std::vector<OpenGroup> open;

    // Per-record channel description, decoded in a second pass.
    struct Slot {
        std::vector<int16_t> ids;
        std::vector<uint32_t> lengths;
        bool has_pixels = false;
        int64_t lw = 0, lh = 0;
        int64_t out_index = -1; // index into `out`, or -1 for records that add no layer
    };
    // One entry per PSD record, in file order. Every record owns its channel
    // bytes, even when it does not add a layer (group dividers).
    std::vector<Slot> pending;

    for (int64_t idx = 0; idx < count; ++idx) {
        const std::string where = " (layer record " + std::to_string(idx) + ")";
        Layer L;
        int32_t top = 0, left = 0, bottom = 0, right = 0;
        uint16_t nch = 0;
        // One field per statement: compound conditions hide which read failed
        // and, on this toolchain, which operand the optimiser kept.
        bool rh_ok = detail::rd_i32(li, top);
        if (rh_ok)
            rh_ok = detail::rd_i32(li, left);
        if (rh_ok)
            rh_ok = detail::rd_i32(li, bottom);
        if (rh_ok)
            rh_ok = detail::rd_i32(li, right);
        if (rh_ok)
            rh_ok = detail::rd_u16(li, nch);
        if (!rh_ok) {
            err = "truncated layer record header" + where;
            return false;
        }
        const int64_t lw = int64_t(right) - int64_t(left);
        const int64_t lh = int64_t(bottom) - int64_t(top);
        const bool has_pixels = lw > 0 && lh > 0;
        if (has_pixels && uint64_t(lw) * uint64_t(lh) > max_pixels) {
            err = "layer bounds too large" + where;
            return !out.empty();
        }
        if (nch > 56) {
            err = "too many channels" + where;
            return !out.empty();
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
        // keybuf has exactly 4 raw bytes, no terminator.
        L.blend_key = std::string(keybuf, 4);
        L.blend_mode = detail::blend_code_from_key(keybuf);
        if (L.blend_key == "norm")
            L.blend_key.clear(); // "norm" is the default, keep the struct tidy
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
                } else if (std::memcmp(bkey, "lfx2", 4) == 0 && blen >= 16) {
                    // Layer effects: we only need an enabled Stroke.
                    Descriptor fx;
                    if (parse_descriptor(extra + bstart, size_t(blen), 0, fx)) {
                        const Descriptor *stroke = fx.get("FrFX");
                        if (stroke != nullptr && stroke->kind == Descriptor::Kind::Object) {
                            const Descriptor *enab = stroke->get("enabb");
                            const bool on = enab != nullptr && enab->as_bool(false);
                            if (on) {
                                L.stroke_on = true;
                                const Descriptor *sz = stroke->get("Sz  ");
                                if (sz != nullptr)
                                    L.stroke_size = sz->as_number(0.0);
                                const Descriptor *clr = stroke->get("Clr ");
                                if (clr != nullptr && clr->kind == Descriptor::Kind::Object) {
                                    const Descriptor *rgb = clr->get("RGBC");
                                    if (rgb != nullptr) {
                                        const Descriptor *rd = rgb->get("Rd  ");
                                        const Descriptor *gn = rgb->get("Grn ");
                                        const Descriptor *bl = rgb->get("Bl  ");
                                        if (rd && gn && bl) {
                                            L.stroke_color[0] = std::max(0.0, std::min(1.0, rd->as_number(0.0) / 255.0));
                                            L.stroke_color[1] = std::max(0.0, std::min(1.0, gn->as_number(0.0) / 255.0));
                                            L.stroke_color[2] = std::max(0.0, std::min(1.0, bl->as_number(0.0) / 255.0));
                                            L.stroke_color[3] = 1.0;
                                        }
                                    }
                                }
                            }
                        }
                    }
                } else if (std::memcmp(bkey, "TySh", 4) == 0 && blen >= 54) {
                    // Type layer: decode the text engine description.
                    TextStyle ts;
                    if (parse_tysh(extra + bstart, size_t(blen), ts)) {
                        L.text = ts.text;
                        L.text_font = ts.font;
                        L.text_size = ts.font_size;
                        for (int ci = 0; ci < 4; ++ci)
                            L.text_color[ci] = ts.color[ci];
                    }
                } else if (is_divider_key(bkey) && blen >= 4) {
                    // lsct divider type (PSD spec):
                    //   0 = any other type (normal layer, NOT a divider)
                    //   1 = open folder  (the "/>" end, contents visible)
                    //   2 = closed folder (the start "<`")
                    //   3 = bounding section divider (Photoshop's group bounds)
                    // 1/2/3 all delimit a group; only 0 is a plain layer.
                    detail::Reader dr(extra + bstart, size_t(blen));
                    uint32_t type = 0;
                    if (detail::rd_u32(dr, type) && type >= 1 && type <= 3) {
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
            // Section divider (psd.h "Groups"). Ours writes the group name on
            // BOTH dividers, so the name decides which one is which; the lsct
            // type (2 below / 1 above) is only the fallback for other writers.
            // Trust the lsct type: 1 = the group's closing record (open folder),
            // 2 / 3 = the group's opening record. Names are NOT used, because
            // Photoshop happily has a group and a layer with the same name.
            const bool is_open_divider = (divider != 1);
            if (is_open_divider) {
                OpenGroup g;
                g.order = out.size();
                g.first_child = out.size() + 1;
                g.name = L.name; // remember the opener's name to pair the close
                open.push_back(std::move(g));
                Layer G;
                G.blend_key = "group";
                G.blend_mode = 'N';
                G.visible = false;
                G.opacity = 100.0f;
                G.left = left;
                G.top = top;
                G.right = right;
                G.bottom = bottom;
                out.push_back(std::move(G));
            } else {
                if (!open.empty()) {
                    const OpenGroup g = std::move(open.back());
                    open.pop_back();
                    if (g.order < out.size()) {
                        Layer &G = out[g.order];
                        G.name = L.name;
                        G.visible = L.visible;
                        // A group never carries pixels: the closing record has
                        // no channel data, and the placeholder must not either.
                        G.rgba.clear();
                        // The closing divider carries the group's bounds; the
                        // opening one Photoshop writes is empty.
                        if (right > left && bottom > top) {
                            G.left = left;
                            G.top = top;
                            G.right = right;
                            G.bottom = bottom;
                        }
                        // Annotate the members now that the group has a name.
                        const size_t first = g.first_child < out.size() ? g.first_child : out.size();
                        for (size_t q = first; q < out.size(); ++q) {
                            if (out[q].blend_key == "group")
                                continue; // nested group placeholder, not a member
                            if (out[q].group_name.empty())
                                out[q].group_name = G.name;
                        }
                    }
                }
            }
            // A divider owns its own channel bytes (usually 4 x 2 by共 of the
            // placeholder length, but a writer may put real data there), so it
            // always gets a slot. The opening divider's group placeholder is
            // bound to it; a closing divider adds no layer.
            Slot slot;
            slot.ids = ids;
            slot.lengths = lengths;
            slot.has_pixels = false;
            if (is_open_divider && !out.empty() && out.back().blend_key == "group")
                slot.out_index = int64_t(out.size()) - 1;
            pending.push_back(std::move(slot));
            continue;
        }

        // Annotations for layers inside a group (psd.h: read-only, the writer
        // ignores them).
        if (!open.empty()) {
            L.group_name = open.back().name;
            L.group_depth = int32_t(open.size());
        }

        // --- channel data -------------------------------------------------
        int lri = -1, lgi = -1, lbi = -1, lai = -1;
        const bool mapped = has_pixels && map_layer_channels(ids.data(), ids.size(), color_mode, lri, lgi, lbi, lai);
        // Planes are indexed by channel-id derived slot only when the ids are
        // itsy bitsy; otherwise slot c is just the channel order. Slot count is
        // bounded by the number of channels.
        size_t max_slot = 0;
        if (mapped) {
            max_slot = size_t(std::max(std::max(lri, lgi), std::max(lbi, lai)));
            if (max_slot >= ids.size())
                max_slot = ids.size() - 1;
        }
        std::unique_ptr<Plane[]> planes;
        if (mapped) {
            planes.reset(new (std::nothrow) Plane[max_slot + 1]);
            if (!planes) {
                err = "out of memory" + where;
                return false;
            }
        }
        // PSD stores ALL record headers first, then ALL channel image data in
        // record order. The channel bodies do not follow their own header, so
        // remember the ids/lengths and decode them in a second pass below.
        Slot slot;
        slot.ids = ids;
        slot.lengths = lengths;
        slot.has_pixels = has_pixels;
        slot.lw = lw;
        slot.lh = lh;
        slot.out_index = int64_t(out.size());
        pending.push_back(std::move(slot));
        out.push_back(std::move(L));
    }

    // --- second pass: channel image data, in record order ------------------
    // Every record consumes its declared bytes; only records that own a layer
    // decode pixels. This keeps the reader in sync with files whose group
    // dividers declare placeholder lengths.
    for (size_t i = 0; i < pending.size(); ++i) {
        Slot &slot = pending[i];
        uint64_t total = 0;
        for (size_t c = 0; c < slot.lengths.size(); ++c)
            total += slot.lengths[c];
        if (total > uint64_t(li.remaining())) {
            // Truncated tail: stop decoding, keep what we have.
            break;
        }
        const size_t record_start = li.pos();
        const size_t record_len = size_t(total);
        const Layer *layer = (slot.out_index >= 0 && size_t(slot.out_index) < out.size())
                                     ? &out[size_t(slot.out_index)]
                                     : nullptr;
        Layer *mutable_layer = (slot.out_index >= 0 && size_t(slot.out_index) < out.size())
                                       ? &out[size_t(slot.out_index)]
                                       : nullptr;
        (void)layer;
        if (!mutable_layer || !slot.has_pixels || slot.ids.empty()) {
            li.skip_n(record_len);
            continue;
        }
        int lri = -1, lgi = -1, lbi = -1, lai = -1;
        if (!map_layer_channels(slot.ids.data(), slot.ids.size(), color_mode, lri, lgi, lbi, lai)) {
            li.skip_n(record_len);
            continue;
        }
        size_t max_slot = size_t(std::max(std::max(lri, lgi), std::max(lbi, lai)));
        if (max_slot >= slot.ids.size())
            max_slot = slot.ids.size() - 1;
        std::unique_ptr<Plane[]> planes(new (std::nothrow) Plane[max_slot + 1]);
        if (!planes) {
            err = "out of memory";
            return false;
        }
        bool decode_ok = true;
        size_t cursor = record_start;
        for (size_t c = 0; c < slot.ids.size(); ++c) {
            const size_t body_len = size_t(slot.lengths[c]);
            if (cursor + body_len > li.size()) {
                decode_ok = false;
                break;
            }
            const uint8_t *body = li.base() + cursor;
            if (c <= max_slot) {
                std::string perr;
                if (!decode_plane_standalone(body, body_len, slot.lw, slot.lh, planes[c], perr)) {
                    decode_ok = false;
                }
            }
            cursor += body_len;
        }
        li.set_pos(record_start + record_len);
        if (!decode_ok) {
            mutable_layer->rgba.clear();
            continue;
        }
        const size_t n = size_t(slot.lw) * size_t(slot.lh);
        const Plane rgba = planes_to_rgba(planes.get(), lri, lgi, lbi, lai, n);
        if (rgba == nullptr) {
            err = "out of memory";
            return false;
        }
        mutable_layer->rgba.assign(rgba.get(), rgba.get() + n * 4);
    }

    // Unclosed "<" dividers: keep the name the divider itself carried.
    for (const OpenGroup &g : open) {
        if (g.order < out.size() && out[g.order].name.empty())
            out[g.order].name = g.name;
    }
    return true;
}

// Reads `channels` planes of the flattened composite starting at `start` and
// turns the colour ones into straight RGBA8.
bool read_composite(const uint8_t *data, size_t size, size_t start, int64_t w, int64_t h, int channels, int ri,
                    int gi, int bi, int ai, uint64_t max_pixels, std::vector<uint8_t> &out, std::string &err)
{
    out.clear();
    if (uint64_t(w) * uint64_t(h) > max_pixels) {
        err = "document too large";
        return false;
    }
    if (start >= size) {
        err = "no composite image data";
        return false;
    }
    detail::Reader cr(data + start, size - start);
    uint16_t compression = 0;
    if (!detail::rd_u16(cr, compression)) {
        err = "truncated composite data";
        return false;
    }
    // The composite stores one plane per channel declared in the file header
    // (Photoshop writes 4 planes for RGB documents: R, G, B, A), so the plane
    // count must come from the header, not from the colour-channel mapping.
    const int color_planes = std::max(std::max(ri, gi), std::max(bi, ai)) + 1;
    const int maxc = (channels >= color_planes && channels <= 56) ? channels : color_planes;
    if (compression == 2 || compression == 3) {
        // ZIP composite: one deflate stream holding all colour planes, each
        // plane w*h bytes, in channel order.
        const size_t avail = cr.remaining();
        if (uint64_t(w) * uint64_t(h) * uint64_t(maxc) > (1ull << 31)) {
            err = "composite too large";
            return false;
        }
        std::vector<uint8_t> inflated(size_t(w) * size_t(h) * size_t(maxc));
        mz_ulong dst_len = mz_ulong(inflated.size());
        const uint8_t *src = cr.ptr(avail);
        if (src == nullptr) {
            err = "truncated composite data";
            return false;
        }
        const int rc = mz_uncompress(inflated.data(), &dst_len, src, mz_ulong(avail));
        if (rc != MZ_OK && rc != MZ_BUF_ERROR) {
            err = "ZIP composite cannot be inflated";
            return false;
        }
        if (size_t(dst_len) < inflated.size())
            std::memset(inflated.data() + dst_len, 0, inflated.size() - size_t(dst_len));
        const size_t plane_w = size_t(w) * size_t(h);
        std::unique_ptr<Plane[]> z_planes(new (std::nothrow) Plane[static_cast<size_t>(maxc)]);
        if (!z_planes) {
            err = "out of memory";
            return false;
        }
        for (int c = 0; c < maxc; ++c) {
            z_planes[size_t(c)] = alloc_plane(plane_w);
            if (plane_w != 0 && z_planes[size_t(c)] == nullptr) {
                err = "out of memory";
                return false;
            }
            if (plane_w != 0)
                std::memcpy(z_planes[size_t(c)].get(), inflated.data() + size_t(c) * plane_w, plane_w);
        }
        const Plane rgba = planes_to_rgba(z_planes.get(), ri, gi, bi, ai, plane_w);
        if (rgba == nullptr) {
            err = "out of memory";
            return false;
        }
        out.assign(rgba.get(), rgba.get() + plane_w * 4);
        return true;
    }
    if (compression != 0 && compression != 1) {
        err = "unsupported composite compression";
        return false;
    }
    // The composite stores the COLOUR planes only, whatever the header channel
    // count says (an RGBA document has 3 composite planes, no alpha plane).
    // Decode exactly max(ri,gi,bi,ai)+1 planes; planes_to_rgba() reports a
    // missing alpha as opaque (255). Anything after that is not read.
    (void)channels;
    std::vector<uint32_t> composite_counts;
    std::unique_ptr<Plane[]> planes(new (std::nothrow) Plane[static_cast<size_t>(maxc)]);
    if (!planes) {
        err = "out of memory";
        return false;
    }
    const uint64_t plane_bytes = uint64_t(w) * uint64_t(h);
    for (int c = 0; c < maxc; ++c) {
        if (compression == 0) {
            if (uint64_t(cr.remaining()) < plane_bytes) {
                err = "truncated raw composite channel";
                return false;
            }
            const uint8_t *p = cr.ptr(size_t(plane_bytes));
            if (p == nullptr) {
                err = "truncated raw composite channel";
                return false;
            }
            planes[size_t(c)] = alloc_plane(size_t(plane_bytes));
            if (plane_bytes != 0 && planes[size_t(c)] == nullptr) {
                err = "out of memory";
                return false;
            }
            if (plane_bytes != 0)
                std::memcpy(planes[size_t(c)].get(), p, size_t(plane_bytes));
        } else {
            // RLE composite: the 2-byte compression tag is stored ONCE for the
            // whole image, followed by the row-count table of ALL planes
            // (h x channels entries) and only then the packed rows.
            std::vector<uint32_t> counts;
            if (c == 0) {
                if (uint64_t(h) * uint64_t(maxc) > uint64_t(cr.remaining()) / 2u) {
                    err = "truncated composite row counts";
                    return false;
                }
                counts.resize(size_t(h) * size_t(maxc));
                for (size_t k = 0; k < counts.size(); ++k) {
                    uint16_t v = 0;
                    if (!detail::rd_u16(cr, v)) {
                        err = "truncated composite row counts";
                        return false;
                    }
                    counts[k] = v;
                }
                composite_counts = std::move(counts);
            }
            if (composite_counts.size() != size_t(h) * size_t(maxc)) {
                err = "composite row count table mismatch";
                return false;
            }
            std::vector<uint32_t> mine(composite_counts.begin() + ptrdiff_t(size_t(c) * size_t(h)),
                                       composite_counts.begin() + ptrdiff_t(size_t(c + 1) * size_t(h)));
            const PlaneResult pr = read_rle_rows_nocounts(cr, w, h, mine, planes[size_t(c)], err);
            if (pr == PlaneResult::Error)
                return false;
        }
    }
    const Plane rgba = planes_to_rgba(planes.get(), ri, gi, bi, ai, size_t(plane_bytes));
    if (rgba == nullptr) {
        err = "out of memory";
        return false;
    }
    try {
        out.assign(rgba.get(), rgba.get() + size_t(plane_bytes) * 4);
    } catch (...) {
        err = "out of memory";
        return false;
    }
    return true;
}

// Does `at` hold a legal composite compression tag for an RGB file?
bool has_composite_tag(const uint8_t *data, size_t size, size_t at)
{
    if (at >= size || size - at < 2)
        return false;
    const unsigned tag = (unsigned(data[at]) << 8) | unsigned(data[at + 1]);
    return tag == 0 || tag == 1;
}

bool parse_document(const uint8_t *data, size_t size, Document &out, std::string &error, const ReadOptions &opts)
{
    if (size < 26 || std::memcmp(data, "8BPS", 4) != 0) {
        error = "not a PSD file (bad signature)";
        return false;
    }

    // --- fixed header ------------------------------------------------------
    if (size < 40) {
        error = "truncated PSD header";
        return false;
    }

    // --- header fields -----------------------------------------------------
    // Header layout (offsets): 4 sig, 2 version, 6 reserved, 2 channels,
    // 4 height, 4 width, 2 depth, 2 colour mode = 26 bytes.
    const uint64_t version = rd_be(data + 4, 2);
    const uint64_t channels = rd_be(data + 12, 2);
    const uint64_t height = rd_be(data + 14, 4);
    const uint64_t width = rd_be(data + 18, 4);
    const uint64_t depth = rd_be(data + 22, 2);
    const uint64_t cmode = rd_be(data + 24, 2);
    if (version == 2) {
        error = "PSB (large document) files are not supported";
        return false;
    }
    if (version != 1) {
        error = "unsupported PSD version";
        return false;
    }
    if (width == 0 || height == 0 || width > 0x7FFFFFFFull || height > 0x7FFFFFFFull) {
        error = "document has zero or invalid size";
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
    if (width * height > opts.max_pixels) {
        error = "document too large for this reader";
        return false;
    }
    out.width = int32_t(width);
    out.height = int32_t(height);
    out.depth = uint16_t(depth);
    out.channels = uint16_t(channels);
    const int color_mode = int(cmode);
    const size_t nchan = size_t(channels);
    const int64_t W = int64_t(width), H = int64_t(height);

    // --- colour mode data + image resources (both only skipped) -----------
    size_t p = 26;
    for (int sec = 0; sec < 2; ++sec) {
        if (size - p < 4 || (sec == 0 && false)) {
            error = sec == 0 ? "truncated colour mode data" : "truncated image resources";
            return false;
        }
        const uint64_t len = rd_be(data + p, 4);
        p += 4;
        if (len > size - p) {
            error = sec == 0 ? "truncated colour mode data" : "truncated image resources";
            return false;
        }
        p += size_t(len);
    }

    // --- layer & mask information ------------------------------------------
    // p now points at the 4-byte layer & mask section length. The composite
    // starts at the end of that whole section (layer info + global layer mask
    // info); the layer-info sub-length bounds only the records.
    if (size - p < 4) {
        error = "truncated layer and mask information";
        return false;
    }
    const uint64_t lam_len = rd_be(data + p, 4);
    const size_t layer_start = p; // position of the length field
    p += 4;
    size_t layout_end = size; // composite start; size == none
    if (lam_len > size - p) {
        error = "truncated layer and mask information";
        return false;
    }
    layout_end = p + size_t(lam_len);

    // Layer info block: its own length field, then the records.
    bool have_layer_info = false;
    size_t li_start = 0, li_len = 0;
    if (lam_len >= 4 && size - p >= 4) {
        // The layer-info length must not run past the end of the section, and
        // must be able to hold the 2-byte layer count.
        const uint64_t li_decl = rd_be(data + p, 4);
        const uint64_t avail = lam_len - 4;
        if (li_decl <= avail && li_decl >= 2) {
            li_start = p + 4;
            li_len = size_t(li_decl);
            have_layer_info = true;
        }
    }
    (void)layer_start;
    // Tolerant fallback: when the declared end of the section does not hold a
    // legal compression tag (a writer that omitted the global layer mask info
    // or declared a wrong length), use the end of the layer-info block.
    if (have_layer_info && layout_end < size && !has_composite_tag(data, size, layout_end)) {
        const size_t alt = li_start + li_len;
        if (alt != layout_end && has_composite_tag(data, size, alt))
            layout_end = alt;
    }

    // --- composite ---------------------------------------------------------
    int ri = 0, gi = 1, bi = 2, ai = -1;
    const bool mode_ok = map_composite_channels(color_mode, int(nchan), ri, gi, bi, ai);
    std::string composite_error;
    if (opts.read_composite) {
        if (!mode_ok) {
            composite_error = "colour mode cannot be converted to RGB";
        } else if (layout_end >= size) {
            composite_error = "no composite image data";
        } else {
            // Pass the header channel count: the composite holds one plane per
            // header channel (Photoshop writes 4 planes for an RGB document).
            const int comp_channels = int(nchan);
            if (!read_composite(data, size, layout_end, W, H, comp_channels, ri, gi, bi, ai, opts.max_pixels,
                                out.composite, composite_error))
                out.composite.clear();
        }
        if (!out.has_composite())
            out.composite.clear();
    }

    // --- layers -------------------------------------------------------------
    out.layers.clear();
    std::string layer_error;
    if (opts.read_layers && have_layer_info && li_len > 0) {
        // parse_layers() parses straight out of the private file copy; every
        // read it performs is bounds-checked against `li_len`.
        if (!parse_layers(data + li_start, li_len, color_mode, opts.max_pixels, W, H, out.layers, layer_error)) {
            out.layers.clear();
        }
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
    detail::HeapBuffer copy;
    if (!copy.reset(size)) {
        error = "out of memory";
        return false;
    }
    detail::opaque_copy_bytes(copy.data(), data, size);
    return parse_document(copy.data(), size, out, error, opts);
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
            if (size != 0)
                detail::opaque_copy_bytes(nb, payload.get(), size);
            payload.reset(nb);
            cap = ncap;
        }
        if (got != 0)
            detail::opaque_copy_bytes(payload.get() + size, chunk, got);
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
