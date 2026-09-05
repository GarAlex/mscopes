//
// Framebuffer.h — a simple RGBA float pixel buffer the effect host operates on.
//
// CPU-first and portable (this is the "proof" renderer). The interface is kept
// deliberately shader-friendly (normalized 0..1 RGBA, bilinear sampling) so a
// Metal texture backend can replace it behind the same Effect ABI later.
//
#pragma once
#include <vector>
#include <cstddef>
#include <algorithm>
#include <new>
#include "../platform/mem.h"

namespace viz {

// Pixel storage is page-aligned, page-multiple memory (platform/mem.h) so
// the GPU backend can wrap it in an MTLBuffer WITHOUT copying (Metal's
// newBufferWithBytesNoCopy contract): the GPU then reads and writes a
// framebuffer's pixels in place and the old per-boundary 15 MB upload and
// download simply don't exist. The hook lets the backend drop its wrapper
// before the memory is released.
inline void (*gFramebufferMemoryFreed)(const void* p) = nullptr;

template <class T>
struct PageAllocator {
    using value_type = T;
    static constexpr size_t kPage = platform::kPage;

    PageAllocator() = default;
    template <class U> PageAllocator(const PageAllocator<U>&) {}

    static size_t bytesFor(size_t n) { return platform::pageRound(n * sizeof(T)); }
    T* allocate(size_t n) {
        void* p = platform::pageAlloc(bytesFor(n));
        if (!p) throw std::bad_alloc();
        return static_cast<T*>(p);
    }
    void deallocate(T* p, size_t n) {
        if (gFramebufferMemoryFreed) gFramebufferMemoryFreed(p);
        platform::pageFree(p, bytesFor(n));
    }
    template <class U> bool operator==(const PageAllocator<U>&) const { return true; }
    template <class U> bool operator!=(const PageAllocator<U>&) const { return false; }
};

// Pixel storage type; effects that swap() a scratch buffer into a framebuffer
// declare the scratch with this so the storage stays wrappable.
using PixelVector = std::vector<float, PageAllocator<float>>;

// 8-bit truncation. AVS's framebuffer was 8-bit and every resample and blend
// floored its integer math, which quietly bled energy out of feedback loops —
// presets that draw additively into a Movement/Blur loop only look right
// because of that decay. Classic effects quantize their output through this
// (the GPU kernels do the same) so the float pipeline decays identically.
inline float q8(float v) {
    v = std::min(1.f, std::max(0.f, v));
    return (float)(int)(v * 255.f) * (1.f / 255.f);
}
inline void quantize8(PixelVector& px) {
    for (size_t i = 0; i + 3 < px.size(); i += 4) {
        px[i] = q8(px[i]); px[i + 1] = q8(px[i + 1]); px[i + 2] = q8(px[i + 2]);
    }
}

struct Framebuffer {
    int w = 0, h = 0;
    PixelVector px;                     // rgba, row-major, size w*h*4

    // Allocates (zeroed) storage for a new size. A no-op when the size is
    // already right: callers use it as "make sure this buffer fits" every
    // frame, and a persistent buffer (an Effect List's) must keep its pixels
    // — an earlier version re-zeroed unconditionally, which cost a 15 MB
    // memset per list per frame and wiped Ignore-input lists' feedback.
    void resize(int W, int H) {
        if (w == W && h == H && px.size() == (size_t)W * H * 4) return;
        w = W; h = H; px.assign((size_t)W * H * 4, 0.f);
    }
    void clear(float r=0,float g=0,float b=0,float a=1) {
        for (size_t i = 0; i < px.size(); i += 4) { px[i]=r; px[i+1]=g; px[i+2]=b; px[i+3]=a; }
    }
    inline float* at(int x, int y)             { return &px[((size_t)y * w + x) * 4]; }
    inline const float* at(int x, int y) const { return &px[((size_t)y * w + x) * 4]; }

    // Additive blend of an RGB color at an integer pixel (clamped, alpha kept 1).
    inline void addClamped(int x, int y, float r, float g, float b) {
        if (x < 0 || y < 0 || x >= w || y >= h) return;
        float* p = at(x, y);
        p[0] = std::min(1.f, p[0] + r);
        p[1] = std::min(1.f, p[1] + g);
        p[2] = std::min(1.f, p[2] + b);
    }

    // Bilinear sample at pixel coordinates (clamped to edges). out = rgba.
    void sample(float fx, float fy, float out[4]) const {
        fx = std::clamp(fx, 0.f, (float)(w - 1));
        fy = std::clamp(fy, 0.f, (float)(h - 1));
        int x0 = (int)fx, y0 = (int)fy;
        int x1 = std::min(x0 + 1, w - 1), y1 = std::min(y0 + 1, h - 1);
        float tx = fx - x0, ty = fy - y0;
        const float* p00 = at(x0, y0); const float* p10 = at(x1, y0);
        const float* p01 = at(x0, y1); const float* p11 = at(x1, y1);
        for (int c = 0; c < 4; ++c) {
            float a = p00[c] + (p10[c] - p00[c]) * tx;
            float b = p01[c] + (p11[c] - p01[c]) * tx;
            out[c] = a + (b - a) * ty;
        }
    }
};

} // namespace viz
