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
#include <limits>

namespace psd {

namespace {

constexpr int32_t kMaxDim = 30000;     // Photoshop's PSD (not PSB) limit
constexpr size_t kMaxRecords = 0x7FFF; // signed 16-bit layer count
constexpr size_t kMaxPascal = 255;

// Section divider types as stored in the lsct block.
constexpr uint32_t kDivOther = 0;
constexpr uint32_t kDivOpen = 1;   // "</" show-all, above the group contents
constexpr uint32_t kDivClosed = 2; // "<"  hide-all, below the group contents

// One entry of the flat record list that ends up in the layer-info section.
struct Record {
    const Layer *layer = nullptr; // null for divider records
    bool is_divider = false;
    uint32_t divider = kDivOther;
    std::string name;             // divider name (PSD section name)
};

bool valid_layer_bounds(const Layer &L)
{
    return L.left >= -kMaxDim && L.top >= -kMaxDim && L.right <= kMaxDim && L.bottom <= kMaxDim &&
           L.left <= L.right && L.top <= L.bottom;
}

bool is_group(const Layer &L) { return L.blend_key == "group"; }

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
void split_premul(const uint8_t *rgba, int32_t w, int32_t h, std::vector<std::vector<uint8_t>> &out)
{
    const size_t n = size_t(w) * size_t(h);
    out.assign(4, std::vector<uint8_t>(n, 0));
    std::vector<uint8_t> &A = out[0], &R = out[1], &G = out[2], &B = out[3];
    for (size_t i = 0; i < n; ++i) {
        const uint8_t a = rgba[i * 4 + 3];
        A[i] = a;
        R[i] = detail::premul(rgba[i * 4 + 0], a);
        G[i] = detail::premul(rgba[i * 4 + 1], a);
        B[i] = detail::premul(rgba[i * 4 + 2], a);
    }
}

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
    fprintf(stderr, "WCR in t=%d l=%d b=%d r=%d nch=%zu size=%zu\n", top, left, bottom, right, chan_ids.size(),
            w.buf.size());
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
    fprintf(stderr, "WCR out size=%zu bytes:", w.buf.size());
    for (size_t q = 0; q < 32 && q < w.buf.size(); ++q)
        fprintf(stderr, " %u", (unsigned)w.buf.data()[q]);
    fprintf(stderr, "\n");
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

// Flattens the record list (bottom-most first) into straight RGBA8.
void flatten(const std::vector<Record> &records, int32_t W, int32_t H, std::vector<uint8_t> &out)
{
    out.assign(size_t(W) * size_t(H) * 4, 0);
    if (out.empty())
        return;
    uint8_t *dst = out.data();
    for (const Record &rec : records) {
        if (rec.is_divider || rec.layer == nullptr)
            continue;
        const Layer &L = *rec.layer;
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
                    const float v = (float(s[c]) * sa + float(d[c]) * da * (1.0f - sa)) / oa;
                    d[c] = uint8_t(v + 0.5f);
                }
                d[3] = uint8_t(oa * 255.0f + 0.5f);
            }
        }
    }
}

// Writes one RLE composite plane: per-row byte counts first, then the rows.
// NOTE: the 2-byte compression tag is written once by the caller, not here.
void append_composite_plane(detail::Writer &w, const std::vector<uint8_t> &plane, int32_t W, int32_t H)
{
    if (W <= 0 || H <= 0 || plane.empty())
        return;
    std::vector<std::vector<uint8_t>> rows(static_cast<size_t>(H));
    for (int32_t y = 0; y < H; ++y) {
        rows[size_t(y)] = rle_encode(plane.data() + size_t(y) * size_t(W), size_t(W));
        if (rows[size_t(y)].size() > 0xFFFFu)
            rows[size_t(y)].resize(0xFFFFu);
    }
    for (int32_t y = 0; y < H; ++y)
        w.u16(uint16_t(rows[size_t(y)].size()));
    for (int32_t y = 0; y < H; ++y)
        w.bytes(rows[size_t(y)].data(), rows[size_t(y)].size());
}

} // namespace

bool serialize_to(detail::ByteBuf &out, const Document &doc, std::string &error)
{
    error.clear();
    if (doc.width <= 0 || doc.height <= 0 || doc.width > kMaxDim || doc.height > kMaxDim) {
        error = "invalid document size";
        return false;
    }
    const int32_t W = doc.width, H = doc.height;

    // --- validate layers ---------------------------------------------------
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

    // --- record list (bottom-most first), see the contract in psd.h --------
    std::vector<Record> records;
    records.reserve(doc.layers.size() + 8);
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
        const bool has_content = j > i + 1;
        {
            Record close;
            close.is_divider = true;
            close.divider = kDivClosed;
            close.name = L.name;
            records.push_back(std::move(close));
        }
        for (size_t k = i + 1; k < j; ++k) {
            Record r;
            r.layer = &doc.layers[k];
            records.push_back(std::move(r));
        }
        if (has_content) {
            Record close;
            close.is_divider = true;
            close.divider = kDivClosed;
            close.name = L.name;
            records.push_back(std::move(close));
        }
        {
            Record open;
            open.is_divider = true;
            open.divider = kDivOpen;
            open.name = L.name;
            records.push_back(std::move(open));
        }
        i = j;
    }
    if (records.size() > kMaxRecords) {
        error = "too many layers for a PSD file";
        return false;
    }

    // --- flat composite (computed first so a failure leaves no file) -------
    std::vector<uint8_t> flat;
    flatten(records, W, H, flat);
    std::vector<std::vector<uint8_t>> flat_planes;
    split_premul(flat.data(), W, H, flat_planes);

    // --- serialise ---------------------------------------------------------
    // The builder writes into caller-provided storage; once it overflows every
    // further write is a no-op and the whole serialisation is reported as a
    // failure (never a silently truncated file).
    // Instrumented write path: the record stream is built into its own buffer
    // first, so it can be shrunk to fit before a single append into `out`.
    // (The direct path showed records beyond the first ones were not what the
    // record builder had produced.)  Record indices are printed once.
    static bool dumped = false;
    if (!dumped) {
        dumped = true;
        for (size_t q = 0; q < records.size(); ++q) {
            const Record &r = records[q];
            if (r.is_divider)
                fprintf(stderr, "WREC %zu div=%u name=%s\n", q, (unsigned)r.divider, r.name.c_str());
            else
                fprintf(stderr, "WREC %zu layer b=%d,%d,%d,%d\n", q, r.layer->left, r.layer->top, r.layer->right,
                        r.layer->bottom);
        }
    }
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
    // Image resources (we write none). NB: PSD writes these lengths as an
    // *unsigned* 32-bit value, but a long-standing Photoshop convention allows
    // the section lengths to be stored as signed; the reader only uses them to
    // skip, so 0 is the safest thing to write here.
    w.u32(0);

    // --- build the layer-info stream into its own buffer -------------------
    // (Rationale: appending records directly into the final buffer left the
    // record stream out of sync with what the record builders produced.)
    const Document *doccp = &doc;
    (void)doccp;
    const size_t lam_at = w.begin_len();
    {
    detail::HeapBuffer li_mem;
    if (!li_mem.reset(size_t(1) << 20)) {
        error = "out of memory";
        return false;
    }
    detail::ByteBuf li_store;
    li_store.attach(li_mem.data(), li_mem.size());
    detail::Writer lw;
    lw.buf.attach(li_mem.data(), li_mem.size());
    const size_t li_at = lw.begin_len();
    lw.i32(-int32_t(records.size())); // Photoshop convention: negative
    // Each record is first built into its own buffer and then copied into the
    // layer-info buffer, so a writer failure can never leave a half record in
    // the stream.
    for (size_t ri = 0; ri < records.size(); ++ri) {
        const Record &rec = records[ri];
        detail::HeapBuffer rec_mem;
        if (!rec_mem.reset(size_t(1) << 16)) {
            error = "out of memory";
            return false;
        }
        detail::Writer rr;
        rr.buf.attach(rec_mem.data(), rec_mem.size());
        if (rec.is_divider) {
            const std::vector<uint8_t> payload = divider_payload(rec.divider);
            write_common_record(rr, 0, 0, 0, 0, {}, {}, "norm", 0, false, false, rec.name, "lsct", payload);
            if (rr.buf.overflowed()) {
                error = "internal buffer too small for a layer record";
                return false;
            }
            for (size_t q = 0; q < rr.size(); ++q)
                lw.u8(rec_mem.data()[q]);
            continue;
        }
        const Layer &L = *rec.layer;
        const bool has_pixels = !L.empty();
        std::vector<std::vector<uint8_t>> planes;
        std::vector<std::vector<uint8_t>> rows;
        std::vector<uint32_t> chan_lengths;
        std::vector<int16_t> chan_ids;
        if (has_pixels) {
            split_premul(L.rgba.data(), L.width(), L.height(), planes);
            chan_ids.assign({-1, 0, 1, 2});
            chan_lengths.resize(4);
            for (size_t c = 0; c < 4; ++c) {
                if (!encode_plane_rows(planes[c], L.width(), L.height(), rows, error)) {
                    error = "layer " + std::to_string(ri) + ": " + error;
                    return false;
                }
                uint64_t len = 2 + uint64_t(L.height()) * 2u; // compression + row counts
                for (const std::vector<uint8_t> &row : rows)
                    len += row.size();
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
        const uint8_t opacity = to_psd_opacity(L.opacity);
        write_common_record(rr, has_pixels ? L.top : 0, has_pixels ? L.left : 0, has_pixels ? L.bottom : 0,
                            has_pixels ? L.right : 0, chan_ids, chan_lengths, blend_key.c_str(), opacity, L.clipped,
                            L.visible, L.name, "luni", name_payload(L.name));
        // Channel data in chan_ids order (-1, 0, 1, 2); the declared length
        // covers everything after the channel's 4-byte length field.
        if (has_pixels) {
            for (size_t c = 0; c < 4; ++c) {
                if (!encode_plane_rows(planes[c], L.width(), L.height(), rows, error))
                    return false;
                append_channel(rr, rows);
            }
        }
        // Commit this record to the layer-info stream.
        if (rr.buf.overflowed()) {
            error = "internal buffer too small for a layer record";
            return false;
        }
        for (size_t q = 0; q < rr.size(); ++q)
            lw.u8(rec_mem.data()[q]);
    }
    lw.end_len(li_at);


    if (w.buf.overflowed() || lw.buf.overflowed()) {
        error = "document too large for one write";
        return false;
    }
    // The layer-info buffer is sized generously up front; write its actual
    // length and append it to the section.
    const size_t li_bytes = lw.size();
    if (li_mem.size() < li_bytes) {
        error = "internal error: layer stream shrank";
        return false;
    }
    {
        const size_t li_at2 = w.begin_len();
        for (size_t q = 0; q < li_bytes; ++q)
            w.u8(li_mem.data()[q]);
        w.end_len(li_at2);
        const size_t gmask_at = w.begin_len();
        w.end_len(gmask_at); // empty global layer mask info
        w.end_len(lam_at);
    }

    // Flattened composite: RLE, colour planes only (no alpha, per contract).
    w.u16(1); // compression: RLE
    std::vector<std::vector<uint8_t>> rows;
    std::vector<uint8_t> plane(size_t(W) * size_t(H));
    for (size_t c = 0; c < 3; ++c) {
        for (size_t i = 0; i < plane.size(); ++i) {
            const uint8_t a = flat[i * 4 + 3];
            plane[i] = detail::premul(flat[i * 4 + size_t(c)], a);
        }
        if (!encode_plane_rows(plane, W, H, rows, error)) {
            error = "composite row too long for PSD";
            return false;
        }
        append_channel(w, rows);
    }

    } // end layer-info buffer scope

    // Hand the finished image to `out`. This is a plain one-pass memmove of a
    // finished, disjoint buffer, so it cannot interact with any aliasing or
    // copy-selection issue the earlier per-field copies were exposed to.
    out.attach(w.buf.data(), out.capacity());
    out.clear();
    if (!out.append(w.buf.data(), w.buf.size())) {
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
        if (serialize_to(view, doc, e2)) {
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
        if (serialize_to(view, doc, e2)) {
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
    FILE *f = std::fopen(path.c_str(), "wb");
    if (f == nullptr) {
        error = "cannot open output file: " + path;
        return false;
    }
    size_t wrote = 0;
    if (view.size() != 0) {
        const uint8_t *src = view.data();
        for (size_t q = 0; q < view.size(); ++q) {
            if (std::fputc(src[q], f) == EOF)
                break;
            ++wrote;
        }
    }
    const bool flush_ok = std::fflush(f) == 0;
    const bool close_ok = std::fclose(f) == 0;
    if (wrote != view.size() || !flush_ok || !close_ok) {
        error = "failed to write whole file";
        std::remove(path.c_str());
        return false;
    }
    return true;
}

} // namespace psd

