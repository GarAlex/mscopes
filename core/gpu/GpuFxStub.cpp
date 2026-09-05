//
// GpuFxStub.cpp — the GPU backend for platforms without one (yet). Every op
// is a no-op and available() is false, so every effect takes its CPU path
// and present() reports failure (the caller keeps its CPU blit). Linked by
// the CMake build on non-Apple platforms; a wgpu backend is the plan there.
//
#include "GpuFx.h"

namespace viz { namespace gpu {

bool available() { return false; }
void* metalDevice() { return nullptr; }
void flush() {}
void invalidateResident(const void*) {}
void syncToCpu(Framebuffer&) {}
void syncToCpuForRead(const Framebuffer&) {}
bool present(const Framebuffer&, void*) { return false; }
void lastPresentTimes(double* a, double* b, double* c) { if (a) *a = 0; if (b) *b = 0; if (c) *c = 0; }

void movement(Framebuffer&, int, bool, bool, int) {}
void rotoBlit(Framebuffer&, float, float, bool) {}
void feedbackWarp(Framebuffer&, const Framebuffer&, float, float, float) {}
void boxBlur(Framebuffer&, int, float) {}
void gridWarp(Framebuffer&, const float*, int, int, bool, bool) {}
void convolve5(Framebuffer&, const float*, float, float, bool, bool, float) {}
void shift(Framebuffer&, float, float, bool, float, bool) {}
void distanceModifier(Framebuffer&, const float*, int, bool, bool) {}
void colorOp(Framebuffer&, int, const float*, int, const float*, int) {}
bool blend(Framebuffer&, const Framebuffer&, int, float) { return false; }

void bloom(Framebuffer&, float, float, float) {}
void kaleidoscope(Framebuffer&, int, float, float) {}
void rgbSplit(Framebuffer&, float, float) {}
void shockwave(Framebuffer&, const float*, float, float) {}
void glitch(Framebuffer&, float, float, float, float) {}
void streaks(Framebuffer&, float, float, float, float, float, float) {}
void lens(Framebuffer&, float) {}
void radialBlur(Framebuffer&, float, int) {}
void edges(Framebuffer&, float, float, float, float, float) {}
void duotone(Framebuffer&, const float*, const float*, float) {}
void shimmer(Framebuffer&, float, float, float) {}
void crt(Framebuffer&, float, float, float, float) {}
void toneMap(Framebuffer&, float, float, float) {}
void vignette(Framebuffer&, float, float, float) {}
bool runCustomKernel(Framebuffer&, const std::string&, const float*, int, std::string* err)
{
    if (err) *err = "no GPU backend on this platform";
    return false;
}

}} // namespace viz::gpu
