// SPDX-License-Identifier: GPL-2.0-or-later
//
// Opaque heap buffer used by the PSD reader.
//
// Allocating (and eventually freeing) the parse buffer in its own translation
// unit keeps the optimiser from seeing how the buffer relates to other
// allocations of the caller; on this machine's clang (21.0) a caller that
// copies layer records out of one local allocation can otherwise be
// transformed into a memcpy whose source range is read from uninitialised
// memory, yielding 0xFF bytes.
#include "impl/psd_common.h"

#include <cstddef>
#include <cstring>
#include <new>

namespace psd {
namespace detail {

uint8_t *bytebuf_new(size_t n)
{
    uint8_t *p = new (std::nothrow) uint8_t[n];
    if (p != nullptr)
        std::memset(p, 0, n);
    return p;
}

void bytebuf_free(uint8_t *p)
{
    delete[] p;
}

void opaque_copy_bytes(uint8_t *dst, const uint8_t *src, size_t n)
{
    if (n != 0 && dst != nullptr && src != nullptr)
        std::memmove(dst, src, n);
}

} // namespace detail
} // namespace psd
