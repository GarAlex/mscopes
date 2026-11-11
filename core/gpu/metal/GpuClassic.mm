//
// GpuClassic.mm — GPU versions of the classic AVS effects.
// How the Metal backend is split up: GpuMetal.h.
//
#include "GpuMetal.h"

namespace viz { namespace gpu {

// ---------------------------------------------------------------------------
//  Classic ports
// ---------------------------------------------------------------------------
// Every op: R = the framebuffer's own (zero-copy) texture, A/B = private
// scratch; the last pass's output is copied back into R by run().

void movement(Framebuffer& fb, int mode, bool wrap, bool blend, int seed)
{
    Ctx* c; id<MTLTexture> R = beginOp(c, fb);
    if (!R) return;
    @autoreleasepool {
        id<MTLTexture> A = c->tex[0];
        Pass p[1];
        p[0] = {@"fx_warp", R, nil, A,
                {(float)mode, wrap ? 1.f : 0.f, blend ? 1.f : 0.f, (float)(seed & 0xffff)}, 4};
        run(c, p, 1, fb.w, fb.h, A, R);
    }
}

void rotoBlit(Framebuffer& fb, float ca, float sa, bool blend)
{
    Ctx* c; id<MTLTexture> R = beginOp(c, fb);
    if (!R) return;
    @autoreleasepool {
        id<MTLTexture> A = c->tex[0];
        Pass p[1];
        p[0] = {@"fx_warp", R, nil, A, {9.f, 1.f, blend ? 1.f : 0.f, 0.f, ca, sa}, 6};
        run(c, p, 1, fb.w, fb.h, A, R);
    }
}

void feedbackWarp(Framebuffer& fb, const Framebuffer& prev, float ca, float sa, float decay)
{
    if (prev.w != fb.w || prev.h != fb.h) return;
    Ctx* c; id<MTLTexture> R = beginOp(c, fb);
    if (!R) return;
    id<MTLTexture> P = fbTexture(c, prev);          // prev is read in place too
    if (!P) return;
    @autoreleasepool {
        Pass p[1];
        p[0] = {@"fx_warp", P, nil, R, {10.f, 0.f, 0.f, 0.f, ca, sa, decay}, 7};
        run(c, p, 1, fb.w, fb.h, R, R);             // writes fb directly (reads only prev)
    }
}

void boxBlur(Framebuffer& fb, int radius, float mix)
{
    if (radius <= 0) return;
    Ctx* c; id<MTLTexture> R = beginOp(c, fb);
    if (!R) return;
    @autoreleasepool {
        id<MTLTexture> A = c->tex[0], B = c->tex[1];
        float r = (float)std::min(radius, 64);
        Pass p[2];
        p[0] = {@"fx_boxblur", R, R, A, {r, 1, 0, 1.f}, 4};
        p[1] = {@"fx_boxblur", A, R, B, {r, 0, 1, mix}, 4};
        run(c, p, 2, fb.w, fb.h, B, R);
    }
}

void gridWarp(Framebuffer& fb, const float* grid, int GX, int GY, bool wrap, bool blend)
{
    if (!grid || GX < 2 || GY < 2) return;
    Ctx* c; id<MTLTexture> R = beginOp(c, fb);
    if (!R) return;
    @autoreleasepool {
        const size_t bytes = (size_t)GX * GY * 3 * sizeof(float);
        id<MTLBuffer> gb = smallBuf(c, bytes);
        if (!gb) return;
        memcpy(gb.contents, grid, bytes);
        id<MTLTexture> A = c->tex[0];
        Pass p[1];
        p[0] = {@"fx_gridwarp", R, nil, A,
                {(float)GX, (float)GY, wrap ? 1.f : 0.f, blend ? 1.f : 0.f}, 4};
        p[0].buf1 = gb;
        run(c, p, 1, fb.w, fb.h, A, R);
    }
}

void shift(Framebuffer& fb, float sx, float sy, bool blend, float alpha, bool bilinear)
{
    onePass(fb, @"fx_shift", {sx, sy, blend ? 1.f : 0.f, alpha, bilinear ? 1.f : 0.f});
}

void distanceModifier(Framebuffer& fb, const float* table, int n, bool blend, bool bilinear)
{
    if (!table || n < 1) return;
    Ctx* c; id<MTLTexture> R = beginOp(c, fb);
    if (!R) return;
    @autoreleasepool {
        const size_t bytes = (size_t)n * sizeof(float);
        id<MTLBuffer> tb = smallBuf(c, bytes);
        if (!tb) return;
        memcpy(tb.contents, table, bytes);
        id<MTLTexture> A = c->tex[0];
        Pass p[1];
        p[0] = {@"fx_distmod", R, nil, A, {(float)n, blend ? 1.f : 0.f, bilinear ? 1.f : 0.f}, 3};
        p[0].buf1 = tb;
        run(c, p, 1, fb.w, fb.h, A, R);
    }
}

void colorOp(Framebuffer& fb, int op, const float* params, int nParams,
             const float* lut, int lutFloats)
{
    Ctx* c; id<MTLTexture> R = beginOp(c, fb);
    if (!R) return;
    @autoreleasepool {
        // buffer(1) is declared by the kernel, so always bind something.
        const size_t bytes = (lut && lutFloats > 0) ? (size_t)lutFloats * sizeof(float) : 16;
        id<MTLBuffer> lb = smallBuf(c, bytes);
        if (!lb) return;
        if (lut && lutFloats > 0) memcpy(lb.contents, lut, bytes);
        id<MTLTexture> A = c->tex[0];
        Pass p[1];
        p[0].kernel = @"fx_colorop"; p[0].in0 = R; p[0].out = A;
        p[0].params[0] = (float)op;
        const int n = std::min(nParams, 39);
        for (int i = 0; i < n; ++i) p[0].params[1 + i] = params ? params[i] : 0.f;
        p[0].nParams = 1 + n;
        p[0].buf1 = lb;
        run(c, p, 1, fb.w, fb.h, A, R);
    }
}

bool blend(Framebuffer& dest, const Framebuffer& src, int mode, float adjustable)
{
    if (mode == 0 || mode == 12 || src.w != dest.w || src.h != dest.h) return false;
    Ctx* c; id<MTLTexture> R = beginOp(c, dest);
    if (!R) return false;
    id<MTLTexture> S = fbTexture(c, src);
    if (!S) return false;
    @autoreleasepool {
        id<MTLTexture> A = c->tex[0];
        Pass p[1];
        p[0] = {@"fx_blend", R, S, A, {(float)mode, adjustable}, 2};
        run(c, p, 1, dest.w, dest.h, A, R);
    }
    return true;
}

void convolve5(Framebuffer& fb, const float k[25], float invScale, float bias,
               bool absolute, bool wrap, float mix)
{
    Ctx* c; id<MTLTexture> R = beginOp(c, fb);
    if (!R) return;
    @autoreleasepool {
        id<MTLTexture> A = c->tex[0];
        Pass p[1];
        p[0].kernel = @"fx_conv5"; p[0].in0 = R; p[0].out = A;
        float* q = p[0].params;
        q[0] = invScale; q[1] = bias; q[2] = absolute ? 1.f : 0.f; q[3] = wrap ? 1.f : 0.f; q[4] = mix;
        memcpy(q + 5, k, 25 * sizeof(float));
        p[0].nParams = 30;
        run(c, p, 1, fb.w, fb.h, A, R);
    }
}

}} // namespace viz::gpu
