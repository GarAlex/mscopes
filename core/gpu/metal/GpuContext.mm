//
// GpuContext.mm — the Metal device, framebuffer wrapping and pass plumbing.
// How the Metal backend is split up: GpuMetal.h.
//
#include "GpuMetal.h"

namespace viz { namespace gpu {

// ---------------------------------------------------------------------------
//  Kernel libraries (compiled from source at first use — no build-system deps):
//  the modern effects and the classic ports, one pipeline table
// ---------------------------------------------------------------------------
static const char* kKernels =
#include "shaders/Ops.msl.inc"
;

static const char* kClassicKernels =
#include "shaders/Classic.msl.inc"
;

// ---------------------------------------------------------------------------
//  Context
// ---------------------------------------------------------------------------
Ctx* gCtx = nullptr;

void releaseWrap(Wrap& wv)
{
#if !__has_feature(objc_arc)
    [wv.tex release];
    [wv.buf release];
#endif
    wv.tex = nil;
    wv.buf = nil;
}

// Framebuffer allocator hook: the memory behind `p` is about to be unmapped.
static void onFramebufferMemoryFreed(const void* p)
{
    if (!gCtx) return;
    auto it = gCtx->wraps.find(p);
    if (it != gCtx->wraps.end()) {
        gCtx->flush();                             // a pending op may still read/write it
        releaseWrap(it->second);
        gCtx->wraps.erase(it);
    }
}

Ctx* ctx()
{
    static Ctx* c = [] () -> Ctx* {
        if (getenv("MSCOPES_NO_GPU")) return nullptr;      // forced CPU paths
        id<MTLDevice> dev = MTLCreateSystemDefaultDevice();
        if (!dev) return nullptr;
        NSError* err = nil;
        id<MTLLibrary> lib = [dev newLibraryWithSource:@(kKernels) options:nil error:&err];
        if (!lib) {
            NSLog(@"[gpu] kernel compile failed: %@", err);
            return nullptr;
        }
        id<MTLLibrary> lib2 = [dev newLibraryWithSource:@(kClassicKernels) options:nil error:&err];
        if (!lib2) {
            NSLog(@"[gpu] classic-port kernel compile failed: %@", err);
            return nullptr;
        }
        NSMutableDictionary* pipes = [NSMutableDictionary dictionary];
        auto build = [&](id<MTLLibrary> l, NSArray<NSString*>* names) -> bool {
            for (NSString* name in names) {
                id<MTLFunction> fn = [l newFunctionWithName:name];
                id<MTLComputePipelineState> ps = fn ? [dev newComputePipelineStateWithFunction:fn error:&err] : nil;
                if (!ps) { NSLog(@"[gpu] pipeline %@ failed: %@", name, err); return false; }
                pipes[name] = ps;
            }
            return true;
        };
        if (!build(lib, @[@"fx_threshold", @"fx_blur", @"fx_bloom_combine",
                          @"fx_kaleido", @"fx_rgbsplit",
                          @"fx_tonemap", @"fx_vignette", @"fx_trails",
                          @"fx_shockwave", @"fx_glitch",
                          @"fx_streak_combine", @"fx_crt", @"fx_shimmer",
                          @"fx_lens", @"fx_radialblur", @"fx_edges", @"fx_duotone"]))
            return nullptr;
        if (!build(lib2, @[@"fx_warp", @"fx_boxblur", @"fx_gridwarp", @"fx_conv5",
                           @"fx_shift", @"fx_distmod", @"fx_colorop", @"fx_blend"]))
            return nullptr;
        Ctx* c = new Ctx;
        c->dev = dev;
        c->queue = [dev newCommandQueue];
        c->pipes = pipes;
        gCtx = c;
        gFramebufferMemoryFreed = onFramebufferMemoryFreed;
        return c;
    }();
    return c;
}

bool available()
{
    Ctx* c = ctx();
    return c != nullptr && !c->broken;
}

void* metalDevice()
{
    Ctx* c = ctx();
    return c ? (__bridge void*)c->dev : nullptr;
}

// ---------------------------------------------------------------------------
//  Pass plumbing
// ---------------------------------------------------------------------------
// The texture aliasing fb's pixels (created on first use, cached by pointer).
id<MTLTexture> fbTexture(Ctx* c, const Framebuffer& fb)
{
    const void* p = fb.px.data();
    if (!p || fb.w <= 0 || fb.h <= 0 || fb.px.size() < (size_t)fb.w * fb.h * 4) return nil;
    const size_t bytes = PageAllocator<float>::bytesFor((size_t)fb.w * fb.h * 4);
    auto it = c->wraps.find(p);
    if (it != c->wraps.end()) {
        Wrap& wv = it->second;
        if (wv.w == fb.w && wv.h == fb.h && wv.bytes == bytes) return wv.tex;
        releaseWrap(wv);                       // same memory, new geometry: rewrap
        c->wraps.erase(it);
    }
    Wrap wv;
    wv.buf = [c->dev newBufferWithBytesNoCopy:const_cast<void*>(p) length:bytes
                                      options:MTLResourceStorageModeShared deallocator:nil];
    if (wv.buf) {
        MTLTextureDescriptor* td = [MTLTextureDescriptor
            texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA32Float
                                         width:(NSUInteger)fb.w height:(NSUInteger)fb.h
                                     mipmapped:NO];
        td.usage = MTLTextureUsageShaderRead | MTLTextureUsageShaderWrite;
        td.storageMode = MTLStorageModeShared;
        wv.tex = [wv.buf newTextureWithDescriptor:td offset:0
                                      bytesPerRow:(NSUInteger)fb.w * 4 * sizeof(float)];
    }
    if (!wv.tex) {
        NSLog(@"[gpu] cannot wrap framebuffer memory as a texture; using CPU paths");
        releaseWrap(wv);
        c->broken = true;
        return nil;
    }
    wv.w = fb.w; wv.h = fb.h; wv.bytes = bytes;
    c->wraps[p] = wv;
    return wv.tex;
}

// Common op prologue: context, scratch pool sized, fb's texture.
id<MTLTexture> beginOp(Ctx*& c, const Framebuffer& fb)
{
    c = ctx();
    if (!c || c->broken || fb.w <= 0 || fb.h <= 0 || !c->ensureSize(fb.w, fb.h)) return nil;
    return fbTexture(c, fb);
}

// The HDR overflow buffer as a texture the effect kernels add to in place,
// or nil when there is none (or it doesn't match the frame). In-place
// read-write access to float textures needs read-write texture tier 2,
// which every Mac GPU this app supports should have; without it the
// effects simply don't feed the overflow (the frame is the same).
id<MTLTexture> overflowTexture(Ctx* c, const Framebuffer& fb, Framebuffer* overflow)
{
    if (!overflow || overflow->w != fb.w || overflow->h != fb.h) return nil;
    if (c->dev.readWriteTextureSupport < MTLReadWriteTextureTier2) return nil;
    return fbTexture(c, *overflow);
}

// The CPU is about to touch pixels: finish whatever the GPU still owes.
void flush()
{
    if (gCtx) gCtx->flush();
}
void invalidateResident(const void*) { flush(); }

bool clear(Framebuffer& fb)
{
    Ctx* c; id<MTLTexture> R = beginOp(c, fb);
    if (!R) return false;
    auto it = c->wraps.find(fb.px.data());
    if (it == c->wraps.end() || !it->second.buf) return false;
    @autoreleasepool {
        id<MTLBlitCommandEncoder> b = [c->cb() blitCommandEncoder];
        [b fillBuffer:it->second.buf range:NSMakeRange(0, (NSUInteger)fb.px.size() * sizeof(float)) value:0];
        [b endEncoding];
        if (++c->pendingOps >= 64) c->flush();
    }
    return true;
}
bool copy(Framebuffer& dst, const Framebuffer& src)
{
    Ctx* c = ctx();
    if (!c || c->broken || src.w <= 0 || src.h <= 0 || dst.w != src.w || dst.h != src.h ||
        dst.px.size() != src.px.size())
        return false;
    if (!fbTexture(c, src) || !fbTexture(c, dst)) return false;   // wraps both
    auto s = c->wraps.find(src.px.data()), d = c->wraps.find(dst.px.data());
    if (s == c->wraps.end() || d == c->wraps.end() || !s->second.buf || !d->second.buf) return false;
    @autoreleasepool {
        id<MTLBlitCommandEncoder> b = [c->cb() blitCommandEncoder];
        [b copyFromBuffer:s->second.buf sourceOffset:0 toBuffer:d->second.buf destinationOffset:0
                     size:(NSUInteger)src.px.size() * sizeof(float)];
        [b endEncoding];
        if (++c->pendingOps >= 64) c->flush();
    }
    return true;
}

void releaseScratch()
{
    Ctx* c = gCtx;
    if (!c) return;
    c->flush();                                    // pending work may still use them
    for (auto& t : c->tex) if (t) WV_RELEASE(t);
    c->w = c->h = 0;
    for (int i = 0; i < Ctx::kGlowMax; ++i) {
        if (c->glowDown[i]) WV_RELEASE(c->glowDown[i]);
        if (c->glowUp[i]) WV_RELEASE(c->glowUp[i]);
    }
    c->glowLevels = c->glowW = c->glowH = 0;
}

void syncToCpu(Framebuffer&) { flush(); }
void syncToCpuForRead(const Framebuffer&) { flush(); }

// Encode the passes into the pending command buffer and copy the last
// output back into the framebuffer's own texture (i.e. its memory). Nothing
// executes until flush() — the next CPU consumer — so consecutive GPU
// effects share one round trip. Kernels gather from their source, so they
// never write in place; the final on-GPU copy is the price (~0.1 ms for
// 15 MB), versus the two CPU-side twiddled copies the old resident-texture
// design paid per boundary.
void run(Ctx* c, Pass* passes, int nPasses, int W, int H,
                id<MTLTexture> result, id<MTLTexture> fbTex)
{
    id<MTLCommandBuffer> cb = c->cb();
    for (int i = 0; i < nPasses; ++i) {
        Pass& p = passes[i];
        id<MTLComputeCommandEncoder> e = [cb computeCommandEncoder];
        [e setComputePipelineState:c->pipes[p.kernel]];
        int ti = 0;
        [e setTexture:p.in0 atIndex:ti++];
        if (p.in1) [e setTexture:p.in1 atIndex:ti++];
        [e setTexture:p.out atIndex:ti];
        [e setBytes:p.params length:sizeof(p.params) atIndex:0];
        if (p.buf1) [e setBuffer:p.buf1 offset:0 atIndex:1];
        MTLSize tg = MTLSizeMake(16, 16, 1);
        MTLSize grid = MTLSizeMake(((NSUInteger)W + 15) / 16, ((NSUInteger)H + 15) / 16, 1);
        [e dispatchThreadgroups:grid threadsPerThreadgroup:tg];
        [e endEncoding];
    }
    if (result != fbTex) {
        id<MTLBlitCommandEncoder> b = [cb blitCommandEncoder];
        [b copyFromTexture:result sourceSlice:0 sourceLevel:0
              sourceOrigin:MTLOriginMake(0, 0, 0)
                sourceSize:MTLSizeMake((NSUInteger)W, (NSUInteger)H, 1)
                 toTexture:fbTex destinationSlice:0 destinationLevel:0
         destinationOrigin:MTLOriginMake(0, 0, 0)];
        [b endEncoding];
    }
    if (++c->pendingOps >= 64) c->flush();     // bound a never-consumed queue
}

// ---------------------------------------------------------------------------
//  Helpers shared by the effect files
// ---------------------------------------------------------------------------
// One-pass ops: kernel R → A, copied back.
void onePass(Framebuffer& fb, NSString* kernel, std::initializer_list<float> params)
{
    Ctx* c; id<MTLTexture> R = beginOp(c, fb);
    if (!R) return;
    @autoreleasepool {
        id<MTLTexture> A = c->tex[0];
        Pass p[1];
        p[0].kernel = kernel; p[0].in0 = R; p[0].out = A;
        int n = 0;
        for (float v : params) if (n < 40) p[0].params[n++] = v;
        p[0].nParams = n;
        run(c, p, 1, fb.w, fb.h, A, R);
    }
}

// A small-data buffer (grid, table, LUT) for one op, from the ring: each
// pending op keeps its own so encoding never overwrites data a queued op
// still reads. Wrapping the ring flushes first.
id<MTLBuffer> smallBuf(Ctx* c, size_t bytes)
{
    if (c->pendingOps >= Ctx::kSmall) c->flush();
    const int i = c->smallIdx++ % Ctx::kSmall;
    id<MTLBuffer> b = c->small[i];
    if (!b || b.length < bytes) {
        id<MTLBuffer> nb = [c->dev newBufferWithLength:std::max(bytes, (size_t)65 * 65 * 3 * sizeof(float))
                                               options:MTLResourceStorageModeShared];
        if (!nb) return nil;
#if !__has_feature(objc_arc)
        [b release];
#endif
        c->small[i] = nb;
        b = nb;
    }
    return b;
}

void dispatch2D(id<MTLCommandBuffer> cb, id<MTLComputePipelineState> ps,
                       id<MTLTexture> t0, id<MTLTexture> t1, id<MTLTexture> t2, const GlowU& u)
{
    id<MTLComputeCommandEncoder> e = [cb computeCommandEncoder];
    [e setComputePipelineState:ps];
    [e setTexture:t0 atIndex:0];
    [e setTexture:t1 atIndex:1];
    if (t2) [e setTexture:t2 atIndex:2];
    [e setBytes:&u length:sizeof(u) atIndex:0];
    id<MTLTexture> out = t2 ? t2 : t1;
    MTLSize tg = MTLSizeMake(16, 16, 1);
    MTLSize grid = MTLSizeMake((out.width + 15) / 16, (out.height + 15) / 16, 1);
    [e dispatchThreadgroups:grid threadsPerThreadgroup:tg];
    [e endEncoding];
}

}} // namespace viz::gpu
