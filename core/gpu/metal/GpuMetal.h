//
// GpuMetal.h — internals shared by the Metal backend's files (not part of
// the engine's interface; that is ../GpuFx.h).
//
//   GpuContext.mm  device, framebuffer wrapping, deferred command buffer, passes
//   GpuPicture.mm  the picture pass (presentation, glow, looks) and its kernels
//   GpuClassic.mm  GPU versions of the classic AVS effects
//   GpuOps.mm      the modern effects, Bloom, tone mapping, pixel-shader kernels
//   GpuSim.mm      simulations: flow particles, fluid, reaction-diffusion
//   shaders/       the Metal source of each shader library
//
#pragma once
#import <Metal/Metal.h>
#import <QuartzCore/CAMetalLayer.h>
#include "../GpuFx.h"
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <initializer_list>
#include <string>
#include <unordered_map>
#include <vector>

namespace viz { namespace gpu {

// A framebuffer's pixels wrapped as a Metal resource: the MTLBuffer aliases
// the vector's mmap'd storage (no copy), the texture is a linear view of it.
struct Wrap {
    id<MTLBuffer> buf = nil;
    id<MTLTexture> tex = nil;
    int w = 0, h = 0;
    size_t bytes = 0;
};

// (Non-ARC caveat, see Pass below: initialize every ObjC pointer.)
struct Ctx {
    id<MTLDevice> dev = nil;
    id<MTLCommandQueue> queue = nil;
    NSDictionary<NSString*, id<MTLComputePipelineState>>* pipes = nil;
    // Picture pass (lazily built by present()/renderPicture()).
    id<MTLLibrary> picLib = nil;
    // One render pipeline per output format: the SDR layer (bgra8), the
    // extended-range layer (rgba16Float), float readback for tests.
    static constexpr int kPicFormats = 4;
    id<MTLRenderPipelineState> picPipe[kPicFormats] = {};
    MTLPixelFormat picPipeFormat[kPicFormats] = {};
    id<MTLComputePipelineState> picPrefilter = nil, picDown = nil, picUp = nil, picBloomAdd = nil,
                                picToneHdr = nil;
    static constexpr int kGlowMax = 6;
    id<MTLTexture> glowDown[kGlowMax] = {};
    id<MTLTexture> glowUp[kGlowMax] = {};
    int glowLevels = 0, glowW = 0, glowH = 0;
    id<MTLTexture> lutTex = nil;
    uint64_t lutVersion = 0;
    int lutSize = 0;
    id<MTLTexture> dummy2D = nil, dummy3D = nil;   // bound when glow/look are off
    bool broken = false;                            // a wrap failed: fall back to CPU paths

    // Deferred execution: ops encode into one pending command buffer and
    // nobody waits until the CPU actually needs pixels (flush()). Encoders
    // in one command buffer execute in order with Metal tracking the
    // resource hazards, so consecutive GPU effects cost one round trip
    // instead of one each.
    id<MTLCommandBuffer> pending = nil;
    int pendingOps = 0;

    // Small per-op data (grids, tables, LUTs): a ring so a pending op keeps
    // its own copy; the ring wrapping forces a flush.
    static constexpr int kSmall = 16;
    id<MTLBuffer> small[kSmall] = {};
    int smallIdx = 0;

    // Private scratch textures for multi-pass ops, recreated on size change.
    int w = 0, h = 0;
    id<MTLTexture> tex[2] = {nil, nil};

    // Wrappers keyed by the framebuffer's pixel pointer; dropped by the
    // Framebuffer allocator hook before the memory is unmapped.
    std::unordered_map<const void*, Wrap> wraps;

    void flush() {
        if (!pending) return;
        [pending commit];
        [pending waitUntilCompleted];
#if !__has_feature(objc_arc)
        [pending release];
#endif
        pending = nil;
        pendingOps = 0;
    }

    id<MTLCommandBuffer> cb() {
        if (!pending) {
            pending = [queue commandBuffer];
#if !__has_feature(objc_arc)
            [pending retain];
#endif
        }
        return pending;
    }

    bool ensureSize(int W, int H) {
        if (w == W && h == H && tex[0]) return true;
        flush();                                   // pending ops may use the old scratch
        MTLTextureDescriptor* td = [MTLTextureDescriptor
            texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA32Float
                                         width:(NSUInteger)W height:(NSUInteger)H
                                     mipmapped:NO];
        td.usage = MTLTextureUsageShaderRead | MTLTextureUsageShaderWrite;
        td.storageMode = MTLStorageModePrivate;
        for (auto& t : tex) {
            t = [dev newTextureWithDescriptor:td];
            if (!t) return false;
        }
        w = W; h = H;
        return true;
    }
};

extern Ctx* gCtx;                                  // set once ctx() succeeds

#if __has_feature(objc_arc)
#define WV_RELEASE(x) ((x) = nil)
#else
#define WV_RELEASE(x) do { [(x) release]; (x) = nil; } while (0)
#endif

// NOTE: every ObjC pointer here carries an explicit initializer. This file
// is built without ARC in the CLI tools (plain clang++), where an
// uninitialized `id` member is stack garbage, not nil — a Pass filled
// field-by-field once crashed in setTexture: on exactly that.
struct Pass {
    NSString* kernel = nil;
    id<MTLTexture> in0 = nil, in1 = nil, out = nil;
    float params[40] = {};              // room for a 5x5 kernel + options
    int nParams = 0;
    id<MTLBuffer> buf1 = nil;           // optional extra input at buffer(1)
};

// Mirror of the shader's PicU / GlowU (all 4-byte floats; the float2 pairs
// come first so offsets match Metal's 8-byte alignment).
struct PicU {
    float srcW, srcH, dstW, dstH;
    float glow, lookStrength, lookSize, vignette;
    float scanlines, grain, bass, beat;
    float frame, pointScale, sharp, dither;
    float hasGlow, hasLook, beatReactive, glowLevels;
    float hdr, peak, linearOut, hasOverflow;
};
struct GlowU { float tx, ty, threshold, knee; };
struct BloomU { float gain, hdr, pad0, pad1; };

// GpuContext.mm
Ctx* ctx();                                         // nullptr: no GPU (or MSCOPES_NO_GPU)
void releaseWrap(Wrap& wv);
id<MTLTexture> fbTexture(Ctx* c, const Framebuffer& fb);
id<MTLTexture> beginOp(Ctx*& c, const Framebuffer& fb);
id<MTLTexture> overflowTexture(Ctx* c, const Framebuffer& fb, Framebuffer* overflow);
void run(Ctx* c, Pass* passes, int nPasses, int W, int H, id<MTLTexture> result, id<MTLTexture> fbTex);
void onePass(Framebuffer& fb, NSString* kernel, std::initializer_list<float> params);
id<MTLBuffer> smallBuf(Ctx* c, size_t bytes);
void dispatch2D(id<MTLCommandBuffer> cb, id<MTLComputePipelineState> ps,
                id<MTLTexture> t0, id<MTLTexture> t1, id<MTLTexture> t2, const GlowU& u);
// GpuPicture.mm
bool ensurePictureCompute(Ctx* c);
bool ensureGlow(Ctx* c, int W, int H);

}} // namespace viz::gpu
