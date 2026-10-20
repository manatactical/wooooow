#ifndef ENGINE_SOURCE_OS_OSMEMORY_H
#define ENGINE_SOURCE_OS_OSMEMORY_H

#include <cstddef>

#if defined(WOWDECOMP_ARCH_64)
// Allocates inside +-2 GiB of the anchor, because M2Array stores a signed 32-bit pointer delta from
// m_data. That bound is the on-disk format, not an x64 limitation. Returns nullptr if no such range is
// free; the caller then fails the load, exactly as the old VirtualQuery scan did.
void* OsAllocWithin2Gb(const void* anchor, size_t size);
void OsFreeWithin2Gb(void* ptr);
#endif

#endif
