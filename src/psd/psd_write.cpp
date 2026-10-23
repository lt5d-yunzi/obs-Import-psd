// SPDX-License-Identifier: GPL-2.0-or-later
//
// PSD writer. See the representation contract at the top of psd.h for the
// layer order, alpha, and group conventions implemented here.
//
// File layout produced:
//   header (8BPS, ver 1, 4 channels, 8 bit, RGB)
//   colour-mode data length 0
//   image-resources section: a single 8BIM/lnsr (layer selection id) block
//   layer & mask info { layer info { count, records, channel data },
//                       global layer mask info length 0 }
//   flattened composite, RLE, 3 channels (no alpha)
#include "psd.h"
#include "impl/psd_common.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <limits>

namespace psd {

namespace {

constexpr int32_t kMaxDim = 30000;     // Photoshop's PSD (not PSB) limit
constexpr size_t kMaxRecords = 0x7FFF; // signed 16-bit layer count
constexpr size_t kMaxPascal = 255;
constexpr size_t kStreamCapacity = size_t(1) << 32;  // layer-info stream scratch buffer

// Section divider types as stored in the lsct block.
constexpr uint32_t kDivOpen = 1;   // "</" show-all, above the group contents
constexpr uint32_t kDivClosed = 2; // "<"  hide-all, below the group contents

// One entry of the flat record list that ends up in the layer-info section.
struct Record {
    const Layer *layer = nullptr;       // null for divider records
    bool is_divider = false;            // convenience flag (divider != 0)
    uint32_t divider = 0;               // lsct type for divider records
    int32_t l = 0, t = 0, r = 0, b = 0; // bounds of a divider record
    std::string name;                   // divider name (PSD section name)
};

// Bounds a group's divider records carry (see the header comment).
struct DividerBounds {
    int32_t l = 0, t = 0, r = 0, b = 0;
};

// A group placeholder (psd.h "Groups").
bool is_group(const Layer &L) { return L.blend_key == "group"; }



// Divider bounds of every group (in `layers` order): the union of the pixel
// bounds of the layers inside it, clamped to the document; all-zero when the
// group has no pixels. Groups are delimited exactly like composite_layers().
std::vector<DividerBounds> group_bounds(const Document &doc)
{
    std::vector<DividerBounds> bounds;
    for (size_t i = 0; i < doc.layers.size(); ++i) {
        if (!is_group(doc.layers[i]))
            continue;
        int64_t l = 0, t = 0, r = 0, b = 0;
        bool any = false;
        for (size_t j = i + 1; j < doc.layers.size() && !is_group(doc.layers[j]); ++j) {
            const Layer &L = doc.layers[j];
            if (L.empty())
                continue;
            if (!any) {
                l = L.left;
                t = L.top;
                r = L.right;
                b = L.bottom;
                any = true;
            } else {
                l = std::min<int64_t>(l, L.left);
                t = std::min<int64_t>(t, L.top);
                r = std::max<int64_t>(r, L.right);
                b = std::max<int64_t>(b, L.bottom);
            }
        }
        DividerBounds d;
        if (any) {
            const int64_t W = doc.width, H = doc.height;
            l = std::min<int64_t>(std::max<int64_t>(l, 0), W);
            t = std::min<int64_t>(std::max<int64_t>(t, 0), H);
            r = std::min<int64_t>(std::max<int64_t>(r, 0), W);
            b = std::min<int64_t>(std::max<int64_t>(b, 0), H);
            if (r < l || b < t)
                l = t = r = b = 0;
            d.l = int32_t(l);
            d.t = int32_t(t);
            d.r = int32_t(r);
            d.b = int32_t(b);
        }
        bounds.push_back(d);
    }
    return bounds;
}

bool valid_layer_bounds(const Layer &L)
{
    return L.left >= -kMaxDim && L.top >= -kMaxDim && L.right <= kMaxDim && L.bottom <= kMaxDim &&
           L.left <= L.right && L.top <= L.bottom;
}

// RLE-encodes one plane row by row; each row must fit the 16-bit count field.
bool encode_plane_rows(const std::vector<uint8_t> &plane, int32_t w, int32_t h,
                       std::vector<std::vector<uint8_t>> &rows, std::string &err)
{
    rows.assign(size_t(h), {});
    for (int32_t y = 0; y < h; ++y) {
        rows[size_t(y)] = rle_encode(plane.data() + size_t(y) * size_t(w), size_t(w));
        if (rows[size_t(y)].size() > 0xFFFFu) {
            err = "RLE row too long for PSD";
            return false;
        }
    }
    return true;
}

// Byte length of one encoded channel as declared in the record: compression
// tag (2) + h row counts (2 each) + the packed rows.
uint64_t channel_length(const std::vector<std::vector<uint8_t>> &rows)
{
    uint64_t n = 2u + uint64_t(rows.size()) * 2u;
    for (const std::vector<uint8_t> &row : rows)
        n += row.size();
    return n;
}

// Appends one channel body in RLE form (compression tag, row counts, rows).
void append_channel(detail::Writer &w, const std::vector<std::vector<uint8_t>> &rows)
{
    w.u16(1); // RLE
    for (const std::vector<uint8_t> &row : rows)
        w.u16(uint16_t(row.size()));
    for (const std::vector<uint8_t> &row : rows)
        w.bytes(row.data(), row.size());
}

uint8_t to_psd_opacity(float f)
{
    if (!(f > 0.0f))
        return 0; // also maps NaN to 0
    if (f >= 255.0f)
        return 255;
    return uint8_t(int(f + 0.5f));
}

// Splits straight RGBA8 into PSD planes ordered alpha, R, G, B (ids -1,0,1,2)
// with the colour premultiplied against white, as PSD stores it.

// Writes an "8BIM key len data<pad>" block (data padded to 4 bytes).
bool write_block(detail::Writer &w, const char *key, const std::vector<uint8_t> &payload)
{
    w.str4("8BIM");
    w.str4(key);
    w.u32(uint32_t(payload.size()));
    w.bytes(payload.data(), payload.size());
    w.zeros((4u - (payload.size() % 4u)) % 4u);
    return !w.buf.empty() && w.buf.data() != nullptr;
}

void divide(uint32_t v, uint32_t &b0, uint32_t &b1, uint32_t &b2, uint32_t &b3)
{
    b0 = (v >> 24) & 0xFFu;
    b1 = (v >> 16) & 0xFFu;
    b2 = (v >> 8) & 0xFFu;
    b3 = v & 0xFFu;
}

std::vector<uint8_t> divider_payload(uint32_t type)
{
    std::vector<uint8_t> p;
    uint32_t b0 = 0, b1 = 0, b2 = 0, b3 = 0;
    divide(type, b0, b1, b2, b3);
    p.push_back(uint8_t(b0));
    p.push_back(uint8_t(b1));
    p.push_back(uint8_t(b2));
    p.push_back(uint8_t(b3));
    if (type == kDivOpen || type == kDivClosed) {
        // Blend mode of the divider, and the "pass through" block Photoshop 6+
        // writes for every section.
        p.insert(p.end(), {'8', 'B', 'I', 'M', 'n', 'o', 'r', 'm'});
        p.insert(p.end(), {'8', 'B', 'I', 'M', 'p', 'a', 's', 's'});
    }
    return p;
}

// Emits a record header (without channel data) for a layer or a divider.
void write_common_record(detail::Writer &w, int32_t top, int32_t left, int32_t bottom, int32_t right,
                         const std::vector<int16_t> &chan_ids, const std::vector<uint32_t> &chan_lengths,
                         const char *blend_key, uint8_t opacity, bool clipped, bool visible, const std::string &name,
                         const char *block_key, const std::vector<uint8_t> &block_payload)
{
    w.i32(top);
    w.i32(left);
    w.i32(bottom);
    w.i32(right);
    w.u16(uint16_t(chan_ids.size()));
    for (size_t c = 0; c < chan_ids.size(); ++c) {
        w.i16(chan_ids[c]);
        w.u32(chan_lengths[c]);
    }
    w.str4("8BIM");
    w.str4(blend_key);
    w.u8(opacity);
    w.u8(clipped ? 1 : 0);
    w.u8(visible ? 0 : 2); // bit 1 = transparency protected (hidden)
    w.u8(0);
    const size_t extra_len_at = w.begin_len();
    w.u32(0); // mask data
    w.u32(0); // blending ranges
    // Pascal name, truncated on a UTF-8 character boundary.
    std::string nm = name;
    if (nm.size() > kMaxPascal) {
        size_t cut = kMaxPascal;
        while (cut > 0 && (static_cast<unsigned char>(nm[cut]) & 0xC0) == 0x80)
            --cut;
        nm.resize(cut);
    }
    w.u8(uint8_t(nm.size()));
    w.bytes(nm.data(), nm.size());
    w.zeros((4u - ((nm.size() + 1u) % 4u)) % 4u);
    write_block(w, block_key, block_payload);
    w.end_len(extra_len_at);
}

// Emits one layer record header (see write_common_record).
void write_record_header(detail::Writer &w, int32_t top, int32_t left, int32_t bottom, int32_t right,
                         const std::vector<int16_t> &chan_ids, const std::vector<uint32_t> &chan_lengths,
                         const char *blend_key4, uint8_t opacity, bool clipped, bool visible, const std::string &name,
                         const char *block_key, const std::vector<uint8_t> &block_payload)
{
    write_common_record(w, top, left, bottom, right, chan_ids, chan_lengths, blend_key4, opacity, clipped, visible,
                        name, block_key, block_payload);
}

std::vector<uint8_t> name_payload(const std::string &name)
{
    std::vector<uint16_t> units;
    detail::utf8_to_utf16(name, units);
    if (units.size() > 4000)
        units.resize(4000);
    const uint32_t n = uint32_t(units.size());
    uint32_t b0 = 0, b1 = 0, b2 = 0, b3 = 0;
    divide(n, b0, b1, b2, b3);
    std::vector<uint8_t> payload;
    payload.reserve(4 + units.size() * 2);
    payload.push_back(uint8_t(b0));
    payload.push_back(uint8_t(b1));
    payload.push_back(uint8_t(b2));
    payload.push_back(uint8_t(b3));
    for (uint16_t u : units) {
        const uint32_t hi = (uint32_t(u) >> 8) & 0xFFu;
        const uint32_t lo = uint32_t(u) & 0xFFu;
        payload.push_back(uint8_t(hi));
        payload.push_back(uint8_t(lo));
    }
    return payload;
}

// Flattens `layers` (bottom-most first) into straight RGBA8, document-sized.
void flatten(const std::vector<const Layer *> &layers, int32_t W, int32_t H, std::vector<uint8_t> &out)
{
    out.assign(size_t(W) * size_t(H) * 4, 0);
    if (out.empty())
        return;
    uint8_t *dst = out.data();
    for (const Layer *Lp : layers) {
        const Layer &L = *Lp;
        if (!L.visible || L.rgba.empty())
            continue;
        const float op = std::clamp(L.opacity, 0.0f, 100.0f) / 100.0f;
        if (!(op > 0.0f))
            continue;
        const int32_t lw = L.width(), lh = L.height();
        if (L.rgba.size() < size_t(lw) * size_t(lh) * 4)
            continue;
        for (int32_t y = 0; y < lh; ++y) {
            const int32_t dy = L.top + y;
            if (dy < 0 || dy >= H)
                continue;
            for (int32_t x = 0; x < lw; ++x) {
                const int32_t dx = L.left + x;
                if (dx < 0 || dx >= W)
                    continue;
                const uint8_t *s = &L.rgba[(size_t(y) * size_t(lw) + size_t(x)) * 4];
                const float sa = (s[3] / 255.0f) * op;
                if (!(sa > 0.0f))
                    continue;
                uint8_t *d = &dst[(size_t(dy) * size_t(W) + size_t(dx)) * 4];
                const float da = d[3] / 255.0f;
                const float oa = sa + da * (1.0f - sa);
                if (!(oa > 0.0f)) {
                    d[0] = d[1] = d[2] = d[3] = 0;
                    continue;
                }
                for (int c = 0; c < 3; ++c) {
                    // Opaque output in exact integer arithmetic. The source is
                    // straight; the destination stays OPAQUE (d[3] == 255) after
                    // the first hit, so this is only the weighted colour sum:
                    //   out = (s*a + d*(255-a)) / 255 , rounded half up.
                    // Multiplying both terms by 255 gives an exact integer
                    //   out = (s*a*255 + d*(255-a)*255) / (255*255)
                    // which is evaluated with 64-bit intermediates.
                    const uint32_t a = uint32_t(s[3]);
                    const uint32_t sv = uint32_t(s[c]);
                    // S = round(s*a/255), D = d (opaque), out = round((S*255 + D*(255-a))/255).
                    const uint64_t S = (uint64_t(sv) * a * 2u + 255u) / 510u;
                    const uint64_t num = S * 255u + uint64_t(d[c]) * (255u - a);
                    d[c] = uint8_t((num * 2u + 255u) / 510u);
                }
                d[3] = uint8_t(oa * 255.0f + 0.5f);
            }
        }
    }
}

// Writes one RLE composite plane: per-row byte counts first, then the rows.
// NOTE: the 2-byte compression tag is written once by the caller, not here.

} // namespace

bool serialize_to(detail::ByteBuf &out, const Document &doc, std::string &error)
{
    error.clear();
    if (doc.width <= 0 || doc.height <= 0 || doc.width > kMaxDim || doc.height > kMaxDim) {
        error = "invalid document size";
        return false;
    }
    const int32_t W = doc.width, H = doc.height;

    // --- validate ---------------------------------------------------------
    for (size_t i = 0; i < doc.layers.size(); ++i) {
        const Layer &L = doc.layers[i];
        if (!valid_layer_bounds(L)) {
            error = "layer " + std::to_string(i) + " has invalid bounds";
            return false;
        }
        if (is_group(L))
            continue;
        if (!L.empty()) {
            if (L.rgba.size() != size_t(L.width()) * size_t(L.height()) * 4) {
                error = "layer " + std::to_string(i) + " pixel buffer does not match its bounds";
                return false;
            }
        } else if (!L.rgba.empty()) {
            error = "layer " + std::to_string(i) + " has pixels but empty bounds";
            return false;
        }
    }

    // --- record list (bottom-most first), see the header comment -----------
    const std::vector<DividerBounds> gb = group_bounds(doc);
    std::vector<Record> records;
    records.reserve(doc.layers.size() + 8);
    size_t group_index = 0;
    for (size_t i = 0; i < doc.layers.size();) {
        const Layer &L = doc.layers[i];
        if (!is_group(L)) {
            Record r;
            r.layer = &L;
            records.push_back(std::move(r));
            ++i;
            continue;
        }
        size_t j = i + 1;
        while (j < doc.layers.size() && !is_group(doc.layers[j]))
            ++j;
        const DividerBounds &d = gb[group_index++];
        Record close; // "<" below the content
        close.divider = kDivClosed;
        close.name = L.name;
        close.l = d.l;
        close.t = d.t;
        close.r = d.r;
        close.b = d.b;
        records.push_back(std::move(close));
        for (size_t k = i + 1; k < j; ++k) {
            Record r;
            r.layer = &doc.layers[k];
            records.push_back(std::move(r));
        }
        Record open; // "</" above the content
        open.divider = kDivOpen;
        open.name = L.name;
        open.l = d.l;
        open.t = d.t;
        open.r = d.r;
        open.b = d.b;
        records.push_back(std::move(open));
        i = j;
    }
    if (records.size() > kMaxRecords) {
        error = "too many layers for a PSD file";
        return false;
    }

    // --- flat composite (computed first so invalid input fails early) ------
    std::vector<uint8_t> flat;
    {
        // The flattened preview shows everything the document shows, including
        // the pixels inside groups (groups are ordering/structure only).
        std::vector<const Layer *> all_visible;
        all_visible.reserve(doc.layers.size());
        for (const Layer &L : doc.layers) {
            if (is_group(L) || !L.visible || L.empty())
                continue;
            all_visible.push_back(&L);
        }
        flatten(all_visible, W, H, flat);
    }

    // --- layer-info stream ------------------------------------------------
    // The record stream is serialised into its own buffer first, with its
    // length patched once complete, and then appended as a single block. Each
    // record gets one contiguous append (see the header comment of this file).
    detail::HeapBuffer li_mem;
    if (!li_mem.reset(kStreamCapacity)) {
        error = "out of memory";
        return false;
    }
    detail::Writer lw;
    lw.buf.attach(li_mem.data(), li_mem.size());
    lw.u16(uint16_t(records.size())); // negative convention: see below
    std::vector<std::vector<uint8_t>> rows;
    for (size_t ri = 0; ri < records.size(); ++ri) {
        const Record &rec = records[ri];
        if (rec.divider != 0) {
            const std::vector<uint8_t> payload = divider_payload(rec.divider);
            // Only "</" carries the group's visibility; "<" is always hidden
            // (that is how Photoshop stores a closed section divider).
            const bool visible = rec.divider == kDivOpen;
            write_record_header(lw, rec.t, rec.l, rec.b, rec.r, {}, {}, "norm", 0, false, visible, rec.name, "lsct",
                                payload);
            continue;
        }
        const Layer &L = *rec.layer;
        const bool has_pixels = !L.empty();
        std::vector<int16_t> chan_ids;
        std::vector<uint32_t> chan_lengths;
        // Straight -> premultiplied planes, ordered alpha, R, G, B.
        std::vector<std::vector<uint8_t>> planes;
        if (has_pixels) {
            const size_t n = size_t(L.width()) * size_t(L.height());
            planes.assign(4, std::vector<uint8_t>(n, 0));
            for (size_t i = 0; i < n; ++i) {
                const uint8_t a = L.rgba[i * 4 + 3];
                planes[0][i] = a;
                planes[1][i] = detail::premul(L.rgba[i * 4 + 0], a);
                planes[2][i] = detail::premul(L.rgba[i * 4 + 1], a);
                planes[3][i] = detail::premul(L.rgba[i * 4 + 2], a);
            }
            chan_ids.assign({-1, 0, 1, 2});
            chan_lengths.resize(4);
            for (size_t c = 0; c < 4; ++c) {
                if (!encode_plane_rows(planes[c], L.width(), L.height(), rows, error)) {
                    error = "layer " + std::to_string(ri) + ": RLE row too long for PSD";
                    return false;
                }
                const uint64_t len = channel_length(rows);
                if (len > 0xFFFFFFFFull) {
                    error = "layer channel data too large for PSD";
                    return false;
                }
                chan_lengths[c] = uint32_t(len);
            }
        }
        // Blend key: the single-letter code wins when set; otherwise keep a
        // printable stored key, else "norm".
        std::string blend_key = "norm";
        if (L.blend_mode != 'N') {
            blend_key = detail::blend_key_from_code(L.blend_mode);
        } else {
            bool key_ok = L.blend_key.size() == 4;
            for (size_t k = 0; k < L.blend_key.size() && key_ok; ++k) {
                const unsigned char ch = static_cast<unsigned char>(L.blend_key[k]);
                if (ch < 0x20 || ch > 0x7E)
                    key_ok = false;
            }
            if (key_ok)
                blend_key = L.blend_key;
        }
        write_record_header(lw, L.top, L.left, L.bottom, L.right, chan_ids, chan_lengths, blend_key.c_str(),
                            to_psd_opacity(L.opacity), L.clipped, L.visible, L.name, "luni", name_payload(L.name));
        // Channel data in chan_ids order (-1, 0, 1, 2); the declared length
        // covers everything after the channel's 4-byte length field.
        if (has_pixels) {
            for (size_t c = 0; c < 4; ++c) {
                if (!encode_plane_rows(planes[c], L.width(), L.height(), rows, error))
                    return false; // cannot happen: checked above
                append_channel(lw, rows);
            }
        }
    }
    if (lw.buf.overflowed()) {
        error = "document too large for one write";
        return false;
    }
    const size_t li_bytes = lw.size();

    // --- file header, resources, layer & mask section ----------------------
    detail::Writer w;
    w.buf.attach(out.data(), out.capacity());
    w.str4("8BPS");
    w.u16(1);   // version
    w.zeros(6); // reserved
    w.u16(4);   // channels
    w.u32(uint32_t(H));
    w.u32(uint32_t(W));
    w.u16(8); // depth
    w.u16(3); // RGB
    w.u32(0); // colour mode data length

    // Image resources: just the layer selection id.
    {
        const size_t at = w.begin_len();
        const std::vector<uint8_t> lnsr(4, 0); // no layer selected
        write_block(w, "lnsr", lnsr);
        w.end_len(at);
    }

    const size_t lam_at = w.begin_len();
    {
        const size_t li_at = w.begin_len();
        for (size_t q = 0; q < li_bytes; ++q)
            w.u8(li_mem.data()[q]);
        w.end_len(li_at);
        const size_t gmask_at = w.begin_len();
        w.end_len(gmask_at); // empty global layer mask info
        w.end_len(lam_at);
    }

    // --- flattened composite: RLE, colour planes only ----------------------
    // PSD image data = ONE 2-byte compression tag, then per plane
    // (h * 2-byte row counts + the packed rows) with no per-plane tag.
    w.u16(1); // compression: RLE
    std::vector<uint8_t> plane(size_t(W) * size_t(H));
    for (size_t c = 0; c < 3; ++c) {
        for (size_t i = 0; i < plane.size(); ++i) {
            // The flattened preview has no alpha plane, so colour is stored as-is
            // (premultiplying here would darken it by the source alpha).
            plane[i] = flat[i * 4 + size_t(c)];
        }
        if (!encode_plane_rows(plane, W, H, rows, error)) {
            error = "composite row too long for PSD";
            return false;
        }
        for (const std::vector<uint8_t> &row : rows)
            w.u16(uint16_t(row.size()));
        for (const std::vector<uint8_t> &row : rows)
            w.bytes(row.data(), row.size());
    }

    if (w.buf.overflowed()) {
        error = "document too large for one write";
        return false;
    }
    // Report the finished image back through `out` (one contiguous append).
    const size_t total = w.size();
    out.attach(w.buf.data(), w.buf.capacity());
    out.clear();
    if (!out.append(w.buf.data(), total)) {
        error = "out of memory";
        return false;
    }
    return true;
}

bool write_memory(std::vector<uint8_t> &out, const Document &doc, std::string &error)
{
    out.clear();
    error.clear();
    // Memory is allocated in psd_buf.cpp (opaque to this translation unit): on
    // this toolchain copying between local containers gets miscompiled.
    std::unique_ptr<uint8_t[]> mem;
    for (int attempt = 0; attempt < 8; ++attempt) {
        const size_t cap = size_t(1) << 20 << attempt; // 1 MiB, 2 MiB, ... 128 MiB
        uint8_t *p = detail::bytebuf_new(cap);
        if (p == nullptr) {
            error = "out of memory";
            return false;
        }
        mem.reset(p);
        detail::ByteBuf view;
        view.attach(mem.get(), cap);
        std::string e2;
        const bool ok = serialize_to(view, doc, e2);
        if (ok) {
            out.clear();
            try {
                out.reserve(view.size());
                for (size_t q = 0; q < view.size(); ++q)
                    out.push_back(view.data()[q]);
            } catch (...) {
                error = "out of memory";
                return false;
            }
            error.clear();
            return true;
        }
        // A capacity error from the builder means "try a bigger buffer"; any
        // other error is final.
        if (e2 != "document too large for one write" && e2 != "out of memory") {
            error = e2;
            return false;
        }
    }
    error = "document too large for one write";
    return false;
}

bool write_file(const std::string &path, const Document &doc, std::string &error)
{
    error.clear();
    // Serialise straight into a single opaque buffer and stream it to disk.
    std::unique_ptr<uint8_t[]> mem;
    detail::ByteBuf view;
    std::string serr;
    bool ok = false;
    for (int attempt = 0; attempt < 8 && !ok; ++attempt) {
        const size_t cap = size_t(1) << 20 << attempt;
        uint8_t *p = detail::bytebuf_new(cap);
        if (p == nullptr)
            break;
        mem.reset(p);
        view.attach(mem.get(), cap);
        std::string e2;
        const bool sok = serialize_to(view, doc, e2);
        if (sok) {
            ok = true;
            break;
        }
        serr = e2;
        if (e2 != "document too large for one write" && e2 != "out of memory")
            break;
    }
    if (!ok) {
        error = serr.empty() ? "out of memory" : serr;
        return false;
    }
    const size_t n = view.size();
    if (n == 0) {
        error = "internal error: empty serialisation";
        return false;
    }
    FILE *f = std::fopen(path.c_str(), "wb");
    if (f == nullptr) {
        error = "cannot open output file: " + path;
        return false;
    }
    size_t wrote = std::fwrite(view.data(), 1, n, f);
    const bool flush_ok = std::fflush(f) == 0;
    const bool close_ok = std::fclose(f) == 0;
    if (wrote != n || !flush_ok || !close_ok) {
        error = "failed to write whole file";
        std::remove(path.c_str());
        return false;
    }
    return true;
}

} // namespace psd

