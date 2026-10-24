// SPDX-License-Identifier: GPL-2.0-or-later
// Minimal, self-contained PSD (Photoshop Document) reader/writer used by obs-psd.
//
// ---------------------------------------------------------------------------
// Representation contract (read this before changing read/write code)
// ---------------------------------------------------------------------------
//
// Layer order
//   `Document::layers` is indexed bottom-most first, i.e. the same order in
//   which Photoshop stores layer records. The flattened composite is rendered
//   by compositing layers[0], layers[1], ... in that order (later = on top).
//   Do not reverse the vector anywhere.
//
// Alpha
//   `Layer::rgba` and `Document::composite` always hold STRAIGHT
//   (non-premultiplied) RGBA8. The PSD file format stores layer pixels
//   PREMULTIPLIED against white (rgb_file = straight * a / 255), so:
//     - the reader multiplies rgb by 255 / a  (and forces rgb = 0 when a == 0),
//     - the writer multiplies rgb by a / 255.
//   Both directions use the same integer rounding rule (see
//   detail::unpremul / detail::premul in psd_common.h):
//     premul(v, a)   = (v * a + 127) / 255
//     unpremul(p, a) = (p * 255 + a / 2) / a, clamped to 255
//   which is an exact inverse for a == 255 and for every premultiplied value,
//   so straight -> premultiplied -> straight round-trips exactly.
//
// Groups (OBS scene groups)
//   A layer with `blend_key == "group"` is an OBS scene group. It is
//   *semantic sugar*: it is never written as a PSD layer record of its own.
//   Its content is every following layer in `layers` up to (but excluding)
//   the next `blend_key == "group"` layer, so groups cannot contain groups
//   (nested groups are not representable in the `layers` vector).
//   When written, the group contributes three records, in this order (bottom
//   to top, which is PSD storage order):
//       1. a hide-all "<"  divider (lsct = 2, closed group)
//       2. the contents: layer records bottom-most first, then the "<" divider
//          again if the contents are not empty (see below)
//       3. a show-all "</" divider (lsct = 1, open group)
//   The group marker's own name is the name given to the "</" divider. When a
//   group has content layers, a second "<" divider named the same is emitted
//   directly above the content, before the "</" divider, because that is the
//   structure Photoshop itself writes for an invisible section-divider pair.
//   If the group has no content, both "<" and "</" divider records carry the
//   name, which lets an empty group survive a round trip.
//
//   The reader does not create group layers; it reads the divider records
//   back into exactly the structure above so that the writer, given the read
//   document again, produces a byte-equivalent file. It additionally fills
//   (information only, not stored by the writer):
//     - Layer::group_name  : the group the layer belongs to, "" if none
//     - Layer::group_depth : nesting depth (0 = top level)
// ---------------------------------------------------------------------------
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace psd {

struct Layer {
    std::string name;
    int32_t left = 0, top = 0, right = 0, bottom = 0; // document coordinates
    float opacity = 100.0f;  // 0..100
    bool visible = true;
    bool clipped = false;
    uint8_t blend_mode = 'N'; // 'N','M','S','D' ...
    std::string blend_key;    // PSD stored blend key (e.g. "norm"), or "group"

    // Straight (non-premultiplied) RGBA8. Empty when the layer has no pixels.
    std::vector<uint8_t> rgba;

    // Set for PSD type layers: the text engine description decoded from the
    // 8BIM/TySh block. `rgba` is empty for these layers (Photoshop stores no
    // pixels), so importers should render `text` instead.
    std::string text;
    std::string text_font;
    double text_size = 0;
    double text_color[4] = {0, 0, 0, 1};

    // Layer effects (lfx2) that matter for rendering a type layer.
    double text_tracking = 0; // 1/1000 em (PSD Tracking)
    bool stroke_on = false;
    double stroke_size = 0;                // px, relative to text_size points
    double stroke_color[4] = {1, 1, 1, 1}; // RGBA 0..1

    // Read-only annotations derived from PSD section dividers (see above).
    // The writer ignores these.
    std::string group_name;
    int32_t group_depth = 0;

    int32_t width() const { return right - left; }
    int32_t height() const { return bottom - top; }
    bool empty() const { return width() <= 0 || height() <= 0; }
};

struct Document {
    int32_t width = 0;
    int32_t height = 0;
    uint16_t depth = 8;   // always 8 on write
    uint16_t channels = 4;
    // Flattened composite as straight RGBA8 (width*height*4).
    std::vector<uint8_t> composite;
    // Layers indexed as stored in the PSD file: index 0 = bottom-most.
    std::vector<Layer> layers;

    bool has_composite() const
    {
        return width > 0 && height > 0 && composite.size() == static_cast<size_t>(width) * size_t(height) * 4;
    }
};

struct ReadOptions {
    bool read_layers = true;
    bool read_composite = true;
    // Safety valve against malicious/corrupt files: document W*H must not
    // exceed this. Also caps the packed size of every individual channel so a
    // single RLE channel can never cause an allocation larger than
    // ~max_pixels bytes.
    uint64_t max_pixels = 1ull << 28;
    // Never true by default. When false (the default) the composite is only
    // generated for RGB documents; CMYK/grayscale/etc. are skipped quietly.
    bool strict = false;
};

// Parses `path`; on failure returns false and fills `error`.
// The function never throws; it is hardened against malformed input and never
// allocates memory proportional to an unchecked size field.
bool read_file(const std::string &path, Document &out, std::string &error, const ReadOptions &opts = {});

// Same as read_file() but parses an in-memory buffer (used by tests/fuzzing).
bool read_memory(const uint8_t *data, size_t size, Document &out, std::string &error, const ReadOptions &opts = {});

// Serialises `doc`. Layers must already be ordered bottom-most first.
// Writes an RGB8 document with a layer&mask section and a flattened RLE
// composite (3 channels, no alpha). Returns false and fills `error` when the
// document cannot be represented; the destination file is left untouched in
// that case.
bool write_file(const std::string &path, const Document &doc, std::string &error);

// Serialises `doc` into `out` (replacing its contents). Same semantics as
// write_file().
bool write_memory(std::vector<uint8_t> &out, const Document &doc, std::string &error);

// RLE (PackBits) helpers shared by reader and writer.
// rle_decode() is bounded: `expected` bytes at most are produced and `src_len`
// bytes at most are consumed; it returns the number of bytes actually decoded
// (which may be < expected when the input is exhausted or malformed).
size_t rle_decode(const uint8_t *src, size_t src_len, size_t expected, std::vector<uint8_t> &out);
std::vector<uint8_t> rle_encode(const uint8_t *src, size_t len);

} // namespace psd
