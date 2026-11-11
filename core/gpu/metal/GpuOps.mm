//
// GpuOps.mm — the modern effects, Bloom, tone mapping and pixel-shader kernels.
// How the Metal backend is split up: GpuMetal.h.
//
#include "GpuMetal.h"

namespace viz { namespace gpu {

// ---------------------------------------------------------------------------
//  Ops
// ---------------------------------------------------------------------------
void bloom(Framebuffer& fb, float threshold, float radius, float intensity, Framebuffer* overflow)
{
    // A multi-scale pyramid (the picture pass's glow chain, shared: both run
    // in order in one command buffer): luminance-threshold prefilter, box
    // downsample to a depth set by the radius, tent upsample-and-add, then
    // frame + glow through the effect's soft rolloff. With an overflow
    // buffer (HDR) the light the rolloff removed above white is kept there.
    Ctx* c; id<MTLTexture> R = beginOp(c, fb);
    if (!R || !ensurePictureCompute(c) || !ensureGlow(c, fb.w, fb.h)) return;
    id<MTLTexture> OV = overflowTexture(c, fb, overflow);
    @autoreleasepool {
        const int L = std::clamp((int)std::lround(std::log2(std::max(2.f, radius))), 2, c->glowLevels);
        id<MTLCommandBuffer> cb = c->cb();
        const float t = std::clamp(threshold, 0.f, 4.f);
        GlowU pu = { 1.f / fb.w, 1.f / fb.h, t, -1.f };      // luminance threshold (see pic_prefilter)
        dispatch2D(cb, c->picPrefilter, R, c->glowDown[0], nil, pu);
        for (int i = 1; i < L; ++i) {
            id<MTLTexture> prev = c->glowDown[i - 1];
            GlowU d = { 1.f / prev.width, 1.f / prev.height, 0, 0 };
            dispatch2D(cb, c->picDown, prev, c->glowDown[i], nil, d);
        }
        id<MTLBlitCommandEncoder> bl = [cb blitCommandEncoder];
        [bl copyFromTexture:c->glowDown[L - 1] toTexture:c->glowUp[L - 1]];
        [bl endEncoding];
        for (int i = L - 2; i >= 0; --i) {
            id<MTLTexture> coarse = c->glowUp[i + 1];
            GlowU d = { 1.f / coarse.width, 1.f / coarse.height, 0, 0 };
            dispatch2D(cb, c->picUp, coarse, c->glowDown[i], c->glowUp[i], d);
        }
        // Combine into scratch A, copied back; the overflow is added in place.
        id<MTLTexture> A = c->tex[0], B = c->tex[1];
        // The pyramid sums L copies of the thresholded light, each spread
        // wider: 1/L keeps the total what the old single blur added, so a
        // preset's feedback settles where it was tuned to.
        BloomU bu = { intensity / (float)L, OV ? 1.f : 0.f, 0, 0 };
        id<MTLComputeCommandEncoder> e = [cb computeCommandEncoder];
        [e setComputePipelineState:c->picBloomAdd];
        [e setTexture:R atIndex:0];
        [e setTexture:c->glowUp[0] atIndex:1];
        [e setTexture:A atIndex:2];
        [e setTexture:(OV ? OV : B) atIndex:3];
        [e setBytes:&bu length:sizeof(bu) atIndex:0];
        [e dispatchThreadgroups:MTLSizeMake(((NSUInteger)fb.w + 15) / 16, ((NSUInteger)fb.h + 15) / 16, 1)
          threadsPerThreadgroup:MTLSizeMake(16, 16, 1)];
        [e endEncoding];
        id<MTLBlitCommandEncoder> b2 = [cb blitCommandEncoder];
        const MTLSize sz = MTLSizeMake((NSUInteger)fb.w, (NSUInteger)fb.h, 1);
        [b2 copyFromTexture:A sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0, 0, 0) sourceSize:sz
                  toTexture:R destinationSlice:0 destinationLevel:0 destinationOrigin:MTLOriginMake(0, 0, 0)];
        [b2 endEncoding];
        if (++c->pendingOps >= 64) c->flush();
    }
}

void kaleidoscope(Framebuffer& fb, int segments, float angle, float zoom)
{
    onePass(fb, @"fx_kaleido", {(float)segments, angle, zoom});
}

void rgbSplit(Framebuffer& fb, float amountPx, float angle)
{
    onePass(fb, @"fx_rgbsplit", {amountPx * std::cos(angle), amountPx * std::sin(angle)});
}

void toneMap(Framebuffer& fb, float exposure, float gamma, float saturation, Framebuffer* overflow)
{
    const float ig = 1.0f / std::max(0.1f, gamma);
    if (!overflow || overflow->w != fb.w || overflow->h != fb.h) {
        onePass(fb, @"fx_tonemap", {exposure, ig, saturation});
        return;
    }
    // HDR: the same result, plus the exposed light above white added to the
    // overflow buffer in place (frame → A, copied back).
    Ctx* c; id<MTLTexture> R = beginOp(c, fb);
    if (!R || !ensurePictureCompute(c)) return;
    id<MTLTexture> OV = overflowTexture(c, fb, overflow);
    if (!OV) {
        onePass(fb, @"fx_tonemap", {exposure, ig, saturation});
        return;
    }
    @autoreleasepool {
        id<MTLCommandBuffer> cb = c->cb();
        id<MTLTexture> A = c->tex[0];
        const float p[4] = {exposure, ig, saturation, 0};
        id<MTLComputeCommandEncoder> e = [cb computeCommandEncoder];
        [e setComputePipelineState:c->picToneHdr];
        [e setTexture:R atIndex:0];
        [e setTexture:A atIndex:1];
        [e setTexture:OV atIndex:2];
        [e setBytes:p length:sizeof(p) atIndex:0];
        [e dispatchThreadgroups:MTLSizeMake(((NSUInteger)fb.w + 15) / 16, ((NSUInteger)fb.h + 15) / 16, 1)
          threadsPerThreadgroup:MTLSizeMake(16, 16, 1)];
        [e endEncoding];
        id<MTLBlitCommandEncoder> b2 = [cb blitCommandEncoder];
        const MTLSize sz = MTLSizeMake((NSUInteger)fb.w, (NSUInteger)fb.h, 1);
        [b2 copyFromTexture:A sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0, 0, 0) sourceSize:sz
                  toTexture:R destinationSlice:0 destinationLevel:0 destinationOrigin:MTLOriginMake(0, 0, 0)];
        [b2 endEncoding];
        if (++c->pendingOps >= 64) c->flush();
    }
}

void vignette(Framebuffer& fb, float inner, float outer, float strength)
{
    onePass(fb, @"fx_vignette", {inner, outer, strength});
}

void trails(Framebuffer& fb, const Framebuffer& prev, float persistence, int mode)
{
    Ctx* c; id<MTLTexture> R = beginOp(c, fb);
    if (!R) return;
    id<MTLTexture> P = fbTexture(c, prev);
    if (!P) return;
    @autoreleasepool {
        id<MTLTexture> A = c->tex[0];
        Pass p[1];
        p[0].kernel = @"fx_trails"; p[0].in0 = R; p[0].in1 = P; p[0].out = A;
        p[0].params[0] = persistence; p[0].params[1] = (float)mode; p[0].nParams = 2;
        run(c, p, 1, fb.w, fb.h, A, R);
    }
}

void shockwave(Framebuffer& fb, const float radii[4], float width, float strength)
{
    onePass(fb, @"fx_shockwave", {radii[0], radii[1], radii[2], radii[3], width, strength});
}

void glitch(Framebuffer& fb, float amount, float seed, float blockPx, float tearPx)
{
    onePass(fb, @"fx_glitch", {amount, seed, blockPx, tearPx});
}

void streaks(Framebuffer& fb, float threshold, float length, float intensity,
             float tintR, float tintG, float tintB)
{
    Ctx* c; id<MTLTexture> R = beginOp(c, fb);
    if (!R) return;
    @autoreleasepool {
        id<MTLTexture> A = c->tex[0], B = c->tex[1];
        // threshold → 3 horizontal blurs with growing sigma = long streak tail
        float s1 = std::max(2.f, length * 0.15f);
        float s2 = std::max(4.f, length * 0.4f);
        float s3 = std::max(8.f, length);
        Pass p[5];
        p[0] = {@"fx_threshold", R, nil, A, {threshold}, 1};
        p[1] = {@"fx_blur", A, nil, B, {s1, std::min(63.f, s1 * 2), 1, 0}, 4};
        p[2] = {@"fx_blur", B, nil, A, {s2, std::min(63.f, s2 * 2), 1, 0}, 4};
        p[3] = {@"fx_blur", A, nil, B, {s3, 63, 1, 0}, 4};
        p[4] = {@"fx_streak_combine", R, B, A, {intensity, tintR, tintG, tintB}, 4};
        run(c, p, 5, fb.w, fb.h, A, R);
    }
}

void lens(Framebuffer& fb, float strength)
{
    onePass(fb, @"fx_lens", {strength});
}

void radialBlur(Framebuffer& fb, float amount, int taps)
{
    onePass(fb, @"fx_radialblur", {amount, (float)taps});
}

void edges(Framebuffer& fb, float glow, float keepSource,
           float r, float g, float b)
{
    onePass(fb, @"fx_edges", {glow, keepSource, r, g, b});
}

void duotone(Framebuffer& fb, const float shadow[3], const float highlight[3], float mixAmt)
{
    onePass(fb, @"fx_duotone", {shadow[0], shadow[1], shadow[2],
                                highlight[0], highlight[1], highlight[2], mixAmt});
}

void shimmer(Framebuffer& fb, float amountPx, float scale, float t)
{
    onePass(fb, @"fx_shimmer", {amountPx, scale, t});
}

void crt(Framebuffer& fb, float curvature, float scanlines, float mask, float corner)
{
    onePass(fb, @"fx_crt", {curvature, scanlines, mask, corner});
}

// ---------------------------------------------------------------------------
//  Custom kernels (EEL→MSL pixel scripts)
// ---------------------------------------------------------------------------
bool runCustomKernel(Framebuffer& fb, const std::string& source,
                     const float* uniforms, int nUniforms, std::string* err)
{
    Ctx* c = ctx();
    if (!c || fb.w == 0) {
        if (err) *err = "Metal unavailable";
        return false;
    }

    struct Entry { id<MTLComputePipelineState> pipe; std::string error; };
    static std::unordered_map<std::string, Entry>* cache =
        new std::unordered_map<std::string, Entry>();

    auto it = cache->find(source);
    if (it == cache->end()) {
        @autoreleasepool {
            Entry e;
            NSError* nserr = nil;
            id<MTLLibrary> lib = [c->dev newLibraryWithSource:@(source.c_str())
                                                      options:nil error:&nserr];
            id<MTLFunction> fn = lib ? [lib newFunctionWithName:@"px_main"] : nil;
            e.pipe = fn ? [c->dev newComputePipelineStateWithFunction:fn error:&nserr] : nil;
            if (!e.pipe)
                e.error = nserr ? nserr.localizedDescription.UTF8String : "kernel compile failed";
            if (cache->size() > 64) cache->clear();     // bound growth
            it = cache->emplace(source, std::move(e)).first;
        }
    }
    if (!it->second.pipe) {
        if (err) *err = it->second.error;
        return false;
    }

    Ctx* c2; id<MTLTexture> R = beginOp(c2, fb);
    if (!R) { if (err) *err = "texture bind failed"; return false; }
    @autoreleasepool {
        id<MTLTexture> A = c->tex[0];
        id<MTLCommandBuffer> cb = c->cb();
        id<MTLComputeCommandEncoder> e = [cb computeCommandEncoder];
        [e setComputePipelineState:it->second.pipe];
        [e setTexture:R atIndex:0];
        [e setTexture:A atIndex:1];
        [e setBytes:uniforms length:(NSUInteger)std::max(1, nUniforms) * sizeof(float) atIndex:0];
        MTLSize tg = MTLSizeMake(16, 16, 1);
        MTLSize grid = MTLSizeMake(((NSUInteger)fb.w + 15) / 16, ((NSUInteger)fb.h + 15) / 16, 1);
        [e dispatchThreadgroups:grid threadsPerThreadgroup:tg];
        [e endEncoding];
        id<MTLBlitCommandEncoder> b = [cb blitCommandEncoder];
        [b copyFromTexture:A sourceSlice:0 sourceLevel:0
              sourceOrigin:MTLOriginMake(0, 0, 0)
                sourceSize:MTLSizeMake((NSUInteger)fb.w, (NSUInteger)fb.h, 1)
                 toTexture:R destinationSlice:0 destinationLevel:0
         destinationOrigin:MTLOriginMake(0, 0, 0)];
        [b endEncoding];
        c->pendingOps++;
    }
    return true;
}

}} // namespace viz::gpu
