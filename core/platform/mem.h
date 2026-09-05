//
// platform/mem.h — page-aligned memory for Framebuffer storage.
//
// The GPU backends wrap a framebuffer's pixels without copying, which needs
// page-aligned, page-multiple memory that stays put: mmap on POSIX,
// VirtualAlloc on Windows. This is one of the three deliberate porting
// seams of the engine (see also fft.h and image.h); everything above it is
// plain C++.
//
#pragma once
#include <cstddef>

#if defined(_WIN32)
#  define WIN32_LEAN_AND_MEAN
#  include <windows.h>
#else
#  include <sys/mman.h>
#endif

namespace viz { namespace platform {

// Apple silicon pages are 16 KB; using that everywhere keeps allocations a
// multiple of every platform's page size.
static constexpr size_t kPage = 16384;

inline size_t pageRound(size_t bytes) {
    size_t b = (bytes + kPage - 1) / kPage * kPage;
    return b ? b : kPage;
}

inline void* pageAlloc(size_t bytes) {
#if defined(_WIN32)
    return VirtualAlloc(nullptr, bytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
#else
    void* p = mmap(nullptr, bytes, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    return p == MAP_FAILED ? nullptr : p;
#endif
}

inline void pageFree(void* p, size_t bytes) {
#if defined(_WIN32)
    (void)bytes;
    VirtualFree(p, 0, MEM_RELEASE);
#else
    munmap(p, bytes);
#endif
}

}} // namespace viz::platform
