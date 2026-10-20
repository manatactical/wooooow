#if defined(WOWDECOMP_ARCH_64)

#include <cstdint>
#include <cstring>
#include <windows.h>
#include "Engine/Source/Os/OsMemory.h"

static PVOID(WINAPI* ResolveVirtualAlloc2())(HANDLE, PVOID, SIZE_T, ULONG, ULONG, MEM_EXTENDED_PARAMETER*, ULONG) {
    static PVOID(WINAPI * address)(HANDLE, PVOID, SIZE_T, ULONG, ULONG, MEM_EXTENDED_PARAMETER*, ULONG);
    static int32_t resolved;

    if (!resolved) {
        HMODULE kernel32 = GetModuleHandleW(L"kernel32.dll");
        address = kernel32 ? reinterpret_cast<decltype(address)>(GetProcAddress(kernel32, "VirtualAlloc2")) : nullptr;
        resolved = 1;
    }

    return address;
}

// The animation-track arrays are file-format M2Arrays: their 32-bit offset is a delta from m_data and is
// overwritten by Relocate() when the sequence file lands. Widening them would change every nested track's
// stride, so the buffer has to sit within +-2 GiB of m_data instead -- which VirtualAlloc2 expresses in one
// call, with no VirtualQuery walk and no silent truncation when the range is full.
void* OsAllocWithin2Gb(const void* anchor, size_t size) {
    SYSTEM_INFO info;
    GetSystemInfo(&info);
    const uintptr_t granularity = info.dwAllocationGranularity;
    const size_t bytes = (size + granularity - 1) & ~(granularity - 1);
    const uintptr_t center = reinterpret_cast<uintptr_t>(anchor);
    const uintptr_t minimum = reinterpret_cast<uintptr_t>(info.lpMinimumApplicationAddress);
    const uintptr_t maximum = reinterpret_cast<uintptr_t>(info.lpMaximumApplicationAddress);
    const uintptr_t lowest = center > minimum + 0x7FFFFFFF ? center - 0x7FFFFFFF : minimum;
    const uintptr_t highest = center + 0x7FFFFFFF < maximum ? center + 0x7FFFFFFF : maximum;

    if (highest <= lowest || highest - lowest < bytes) {
        return nullptr;
    }

    auto virtualAlloc2 = ResolveVirtualAlloc2();

    if (virtualAlloc2) {
        MEM_ADDRESS_REQUIREMENTS constraints;
        constraints.LowestStartingAddress = reinterpret_cast<void*>(lowest);
        constraints.HighestEndingAddress = reinterpret_cast<void*>(highest & ~(granularity - 1));
        constraints.Alignment = 0;

        MEM_EXTENDED_PARAMETER parameter;
        memset(&parameter, 0, sizeof(parameter));
        parameter.Type = MemExtendedParameterAddressRequirements;
        parameter.Pointer = &constraints;

        void* ptr = virtualAlloc2(nullptr, nullptr, bytes, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE, &parameter, 1);

        if (ptr) {
            return ptr;
        }
    }

    for (uintptr_t address = center; address < highest;) {
        MEMORY_BASIC_INFORMATION region;

        if (!VirtualQuery(reinterpret_cast<void*>(address), &region, sizeof(region))) {
            break;
        }

        const uintptr_t base = reinterpret_cast<uintptr_t>(region.BaseAddress);
        const uintptr_t end = base + region.RegionSize;

        if (region.State == MEM_FREE) {
            const uintptr_t candidate = ((base > lowest ? base : lowest) + granularity - 1) & ~(granularity - 1);

            if (candidate + bytes <= end && candidate + bytes <= highest) {
                void* ptr = VirtualAlloc(reinterpret_cast<void*>(candidate), bytes, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);

                if (ptr) {
                    return ptr;
                }
            }
        }

        address = end > address ? end : address + granularity;
    }

    return nullptr;
}

void OsFreeWithin2Gb(void* ptr) {
    if (ptr) {
        VirtualFree(ptr, 0, MEM_RELEASE);
    }
}

#endif
