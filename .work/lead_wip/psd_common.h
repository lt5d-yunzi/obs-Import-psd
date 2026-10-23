// SPDX-License-Identifier: GPL-2.0-or-later
//
// Shared helpers for the PSD reader/writer.
//
// ---------------------------------------------------------------------------
// Compiler hazard on this machine (Apple clang, -O2)
// ---------------------------------------------------------------------------
// Earlier revisions of this plugin were hit by wrong code from the optimiser
// on this host. The countermeasures that are still in place, and why:
//   1. `detail::Writer` never grows storage: the storage is handed in by the
//      caller (ByteBuf::attach) and every append is bounds-checked. A refused
//      append sets `overflowed()`, which the callers use to grow the buffer
//      and retry instead of ever writing past the end.
//   2. The big parse/write scratch buffers are allocated through
//      bytebuf_new()/HeapBuffer, i.e. in another translation unit
//      (psd_buf.cpp). That keeps the optimiser from merging the buffer with
//      unrelated allocations of the caller.
//   3. Final copies of a byte range that must be bit-exact are done element by
//      element (see psd_buf.cpp) rather than through memcpy.
// These are cheap; do not remove them without re-running tests/run_all.sh.
// ---------------------------------------------------------------------------
#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <new>
#include <string>
#include <vector>

namespace psd {
namespace detail {

// Preallocated byte buffer with a hard capacity. The storage is handed in by
// the caller so that nothing is ever resized under a live pointer.
class ByteBuf {
public:
    ByteBuf() = default;
    ByteBuf(const ByteBuf &) = delete;
    ByteBuf &operator=(const ByteBuf &) = delete;

    // Adopts an existing allocation of `cap` bytes (may be null when cap==0).
    void attach(uint8_t *mem, size_t cap)
    {
        mem_ = mem;
        cap_ = cap;
        size_ = 0;
        overflow_ = false;
    }
    uint8_t *data() { return mem_; }
    const uint8_t *data() const { return mem_; }
    size_t size() const { return size_; }
    size_t capacity() const { return cap_; }
    bool empty() const { return size_ == 0; }
    bool fits(size_t n) const { return n <= cap_ - size_; }
    // True once any append was refused (i.e. the storage was too small).
    bool overflowed() const { return overflow_; }
    // Appends n bytes (copied from p, or zeros when p is null). Never
    // reallocates; returns false when n does not fit.
    bool append(const void *p, size_t n)
    {
        if (mem_ == nullptr || n > cap_ - size_) {
            overflow_ = true;
            return false;
        }
        if (n != 0) {
            unsigned char *dst = reinterpret_cast<unsigned char *>(mem_) + size_;
            if (p != nullptr) {
                const unsigned char *src = static_cast<const unsigned char *>(p);
                for (size_t i = 0; i < n; ++i)
                    dst[i] = src[i];
            } else {
                for (size_t i = 0; i < n; ++i)
                    dst[i] = 0;
            }
        }
        size_ += n;
        return true;
    }
    bool zeros(size_t n) { return append(nullptr, n); }
    bool push(uint8_t v) { return append(&v, 1); }
    void clear() { size_ = 0; }

private:
    uint8_t *mem_ = nullptr;
    size_t cap_ = 0;
    size_t size_ = 0;
    bool overflow_ = false;
};

// Heap byte buffer whose storage is allocated by a routine in another
// translation unit (psd_buf.cpp), so the optimiser cannot merge it with other
// allocations of the caller.
uint8_t *bytebuf_new(size_t n);
void bytebuf_free(uint8_t *p);
void opaque_copy_bytes(uint8_t *dst, const uint8_t *src, size_t n);
void opaque_copy_loop(uint8_t *dst, const uint8_t *src, size_t n);

class HeapBuffer;
bool heapbuf_reset(HeapBuffer *b, size_t n);
uint8_t *heapbuf_data(HeapBuffer *b);

class HeapBuffer {
public:
    HeapBuffer() = default;
    HeapBuffer(const HeapBuffer &) = delete;
    HeapBuffer &operator=(const HeapBuffer &) = delete;
    ~HeapBuffer() { bytebuf_free(p_); }
    // (Re)allocates exactly n zeroed bytes.
    bool reset(size_t n)
    {
        bytebuf_free(p_);
        p_ = nullptr;
        n_ = 0;
        if (n == 0)
            return true;
        p_ = bytebuf_new(n);
        if (p_ == nullptr)
            return false;
        n_ = n;
        return true;
    }
    uint8_t *data() { return p_; }
    const uint8_t *data() const { return p_; }
    size_t size() const { return n_; }

private:
    uint8_t *p_ = nullptr;
    size_t n_ = 0;
};

// Sequential reader over a raw buffer. Every accessor is bounds-checked and
// reports failure; nothing ever reads past `size`.
class Reader {
public:
    Reader(const uint8_t *d, size_t n) : data_(d), size_(n) {}

    size_t pos() const { return pos_; }
    size_t size() const { return size_; }
    size_t remaining() const { return pos_ < size_ ? size_ - pos_ : 0; }
    bool at_end() const { return pos_ >= size_; }
    const uint8_t *base() const { return data_; }
    const uint8_t *cur() const { return data_ + pos_; }

    void set_pos(size_t p) { pos_ = p; }
    // Moves to absolute position p; false when p is past the end.
    bool seek(size_t p)
    {
        if (p > size_)
            return false;
        pos_ = p;
        return true;
    }
    // Advances by n; returns n, or 0 when the request does not fit.
    size_t skip_n(size_t n)
    {
        if (n > remaining())
            return 0;
        pos_ += n;
        return n;
    }
    const uint8_t *ptr(size_t n)
    {
        if (n > remaining())
            return nullptr;
        const uint8_t *p = data_ + pos_;
        pos_ += n;
        return p;
    }

private:
    const uint8_t *data_;
    size_t size_;
    size_t pos_ = 0;
};

inline bool rd_u8(Reader &r, uint8_t &v)
{
    if (r.remaining() < 1)
        return false;
    v = r.base()[r.pos()];
    r.set_pos(r.pos() + 1);
    return true;
}
inline bool rd_u16(Reader &r, uint16_t &v)
{
    if (r.remaining() < 2)
        return false;
    const uint8_t *p = r.base() + r.pos();
    const uint64_t be = (uint64_t(p[0]) << 8) | uint64_t(p[1]);
    r.set_pos(r.pos() + 2);
    v = uint16_t(be);
    return true;
}
inline bool rd_i16(Reader &r, int16_t &v)
{
    uint16_t u = 0;
    if (!rd_u16(r, u))
        return false;
    v = int16_t(u < 0x8000u ? int32_t(u) : int32_t(u) - 0x10000);
    return true;
}
inline bool rd_u32(Reader &r, uint32_t &v)
{
    if (r.remaining() < 4)
        return false;
    const uint8_t *p = r.base() + r.pos();
    const uint64_t be =
        (uint64_t(p[0]) << 24) | (uint64_t(p[1]) << 16) | (uint64_t(p[2]) << 8) | uint64_t(p[3]);
    r.set_pos(r.pos() + 4);
    v = uint32_t(be);
    return true;
}
inline bool rd_i32(Reader &r, int32_t &v)
{
    uint32_t u = 0;
    if (!rd_u32(r, u))
        return false;
    v = (u < 0x80000000u) ? int32_t(u) : int32_t(int64_t(u) - int64_t(0x100000000ll));
    return true;
}

// Writer over a ByteBuf. All multi-byte integers are split into their bytes
// first and appended together.
class Writer {
public:
    ByteBuf buf;

    void u8(uint8_t v) { buf.push(v); }
    void u16(uint16_t v)
    {
        uint8_t b[2] = {uint8_t(v >> 8), uint8_t(v & 0xFFu)};
        buf.append(b, 2);
    }
    void u32(uint32_t v)
    {
        uint8_t b[4] = {uint8_t(v >> 24), uint8_t((v >> 16) & 0xFFu), uint8_t((v >> 8) & 0xFFu), uint8_t(v & 0xFFu)};
        buf.append(b, 4);
    }
    void i16(int16_t v) { u16(uint16_t(v)); }
    void i32(int32_t v) { u32(uint32_t(v)); }
    void bytes(const void *p, size_t n) { buf.append(p, n); }
    void zeros(size_t n) { buf.zeros(n); }
    void str4(const char *s) { buf.append(s, 4); }
    size_t size() const { return buf.size(); }

    size_t begin_len()
    {
        const size_t at = buf.size();
        u32(0);
        return at;
    }
    void end_len(size_t at) { patch_u32(at, uint32_t(buf.size() - at - 4)); }
    void patch_u32(size_t at, uint32_t v)
    {
        if (at + 4 > buf.size())
            return;
        uint8_t *p = buf.data() + at;
        p[0] = uint8_t(v >> 24);
        p[1] = uint8_t((v >> 16) & 0xFFu);
        p[2] = uint8_t((v >> 8) & 0xFFu);
        p[3] = uint8_t(v & 0xFFu);
    }
};

// PSD 4-char blend keys <-> the single letter used by OBS / our blend enum.
uint8_t blend_code_from_key(const char *key4);
const char *blend_key_from_code(uint8_t code);

// Straight <-> premultiplied alpha, exact inverses for every well-formed
// (premultiplied) pair, see psd.h.
inline uint8_t premul(uint8_t v, uint8_t a)
{
    if (a == 255)
        return v;
    if (a == 0)
        return 0;
    return uint8_t((uint32_t(v) * uint32_t(a) + 127u) / 255u);
}

inline uint8_t unpremul(uint8_t p, uint8_t a)
{
    if (a == 255)
        return p;
    if (a == 0)
        return 0;
    // Well-formed files have p <= a; clamp corrupt values instead of wrapping.
    const uint32_t v = (uint32_t(p) * 255u + uint32_t(a) / 2u) / uint32_t(a);
    return uint8_t(v > 255u ? 255u : v);
}

// UTF-8 <-> UTF-16. Malformed UTF-8 becomes U+FFFD; lone surrogates in UTF-16
// become U+FFFD.
void utf8_to_utf16(const std::string &s, std::vector<uint16_t> &out);
void utf16_to_utf8(const uint16_t *in, size_t n, std::string &out);

} // namespace detail
} // namespace psd
