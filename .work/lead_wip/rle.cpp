// SPDX-License-Identifier: GPL-2.0-or-later
#include "psd.h"
#include "impl/psd_common.h"

namespace psd {

// PackBits (PSD's RLE) decoder.
//
// Decodes at most `expected` bytes and reads at most `src_len` bytes; each
// iteration advances the input, so it always terminates. `out` is sized to
// exactly `expected` bytes (zero padded when the stream ends early) and the
// number of bytes actually produced is returned.
size_t rle_decode(const uint8_t *src, size_t src_len, size_t expected, std::vector<uint8_t> &out)
{
    out.assign(expected, uint8_t(0));
    if (expected == 0 || src_len == 0 || src == nullptr || out.empty())
        return 0;
    size_t produced = 0;
    size_t i = 0;
    uint8_t *dst = out.data();
    while (i < src_len && produced < expected) {
        const uint8_t code = src[i];
        ++i;
        if (code < 128) {
            // Literal run: code + 1 bytes copied verbatim.
            size_t count = size_t(code) + 1u;
            if (count > src_len - i)
                count = src_len - i;
            if (count > expected - produced)
                count = expected - produced;
            if (count == 0)
                break;
            std::memcpy(dst + produced, src + i, count);
            produced += count;
            i += count;
        } else if (code != 128) {
            // Repeat run: (257 - code) copies of the next byte.
            if (i >= src_len)
                break;
            size_t count = size_t(257u - uint32_t(code));
            if (count > expected - produced)
                count = expected - produced;
            std::memset(dst + produced, src[i], count);
            produced += count;
            ++i;
        }
        // 128 is a no-op in PackBits.
    }
    return produced;
}

// Greedy PackBits encoder with the same run strategy Photoshop uses. Output is
// at most len + ceil(len / 128) bytes.
std::vector<uint8_t> rle_encode(const uint8_t *src, size_t len)
{
    std::vector<uint8_t> out;
    if (len == 0 || src == nullptr)
        return out;
    out.reserve(len / 2 + 16);
    size_t i = 0;
    while (i < len) {
        size_t run = 1;
        while (i + run < len && run < 128 && src[i + run] == src[i])
            ++run;
        if (run >= 2) {
            // Repeat run of `run` (2..128) -> code = 257 - run, then the byte.
            const uint32_t code = 257u - uint32_t(run);
            out.push_back(uint8_t(code));
            out.push_back(src[i]);
            i += run;
            continue;
        }
        // Literal block: until the next run of >= 2, at most 128 bytes.
        const size_t start = i;
        size_t lit = 0;
        while (i < len && lit < 128) {
            if (i + 1 < len && src[i] == src[i + 1])
                break;
            ++i;
            ++lit;
        }
        out.push_back(uint8_t(lit - 1));
        out.insert(out.end(), src + start, src + start + lit);
    }
    return out;
}

} // namespace psd
