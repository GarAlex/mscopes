//
// GpuPicture.mm — the picture pass: presentation, glow, looks.
// How the Metal backend is split up: GpuMetal.h.
//
#include "GpuMetal.h"

namespace viz { namespace gpu {

// ---------------------------------------------------------------------------
//  Presentation
// ---------------------------------------------------------------------------
// Picture pass (Picture.h). One oversized triangle covers the drawable; uv
// is derived from clip space so the framebuffer's bottom-up row order lands
// the right way up without a flip. The glow is a small pyramid computed from
// the frame into private textures; nothing here writes the frame itself.
static const char* kPictureShaders =
#include "shaders/Picture.msl.inc"
;

static double gPresentUploadMs = 0, gPresentDrawableMs = 0, gPresentGpuMs = 0;
void lastPresentTimes(double* u, double* d, double* g)
{
    if (u) *u = gPresentUploadMs;
    if (d) *d = gPresentDrawableMs;
    if (g) *g = gPresentGpuMs;
}

// The picture library, its compute pipelines (glow pyramid, bloom) and the
// dummy textures; shared by the picture pass and the in-stack Bloom.
bool ensurePictureCompute(Ctx* c)
{
    if (c->picLib) return c->picPrefilter && c->picDown && c->picUp && c->picBloomAdd && c->picToneHdr;
    NSError* err = nil;
    c->picLib = [c->dev newLibraryWithSource:@(kPictureShaders) options:nil error:&err];
    if (!c->picLib) { NSLog(@"[gpu] picture shaders failed: %@", err); return false; }
    auto comp = [&](NSString* name) -> id<MTLComputePipelineState> {
        id<MTLFunction> fn = [c->picLib newFunctionWithName:name];
        id<MTLComputePipelineState> ps = fn ? [c->dev newComputePipelineStateWithFunction:fn error:&err] : nil;
#if !__has_feature(objc_arc)
        [fn release];
#endif
        if (!ps) NSLog(@"[gpu] picture pipeline %@ failed: %@", name, err);
        return ps;
    };
    c->picPrefilter = comp(@"pic_prefilter");
    c->picDown = comp(@"pic_down");
    c->picUp = comp(@"pic_up");
    c->picBloomAdd = comp(@"pic_bloom_add");
    c->picToneHdr = comp(@"pic_tonemap_hdr");
    if (!c->picPrefilter || !c->picDown || !c->picUp || !c->picBloomAdd || !c->picToneHdr) return false;

    MTLTextureDescriptor* d2 = [MTLTextureDescriptor
        texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA16Float width:1 height:1 mipmapped:NO];
    d2.usage = MTLTextureUsageShaderRead;
    c->dummy2D = [c->dev newTextureWithDescriptor:d2];
    MTLTextureDescriptor* d3 = [MTLTextureDescriptor new];
    d3.textureType = MTLTextureType3D;
    d3.pixelFormat = MTLPixelFormatRGBA16Float;
    d3.width = d3.height = d3.depth = 1;
    d3.usage = MTLTextureUsageShaderRead;
    c->dummy3D = [c->dev newTextureWithDescriptor:d3];
#if !__has_feature(objc_arc)
    [d3 release];
#endif
    return c->dummy2D && c->dummy3D;
}

// The picture pass's render pipeline for an output format.
static id<MTLRenderPipelineState> picturePipeline(Ctx* c, MTLPixelFormat fmt)
{
    if (!ensurePictureCompute(c)) return nil;
    int freeSlot = -1;
    for (int i = 0; i < Ctx::kPicFormats; ++i) {
        if (c->picPipe[i] && c->picPipeFormat[i] == fmt) return c->picPipe[i];
        if (!c->picPipe[i] && freeSlot < 0) freeSlot = i;
    }
    if (freeSlot < 0) { freeSlot = 0; WV_RELEASE(c->picPipe[0]); }
    NSError* err = nil;
    MTLRenderPipelineDescriptor* d = [MTLRenderPipelineDescriptor new];
    id<MTLFunction> vs = [c->picLib newFunctionWithName:@"pic_vs"];
    id<MTLFunction> fs = [c->picLib newFunctionWithName:@"pic_fs"];
    d.vertexFunction = vs;
    d.fragmentFunction = fs;
    d.colorAttachments[0].pixelFormat = fmt;
    c->picPipe[freeSlot] = [c->dev newRenderPipelineStateWithDescriptor:d error:&err];
#if !__has_feature(objc_arc)
    [vs release]; [fs release]; [d release];
#endif
    if (!c->picPipe[freeSlot]) { NSLog(@"[gpu] picture render pipeline failed: %@", err); return nil; }
    c->picPipeFormat[freeSlot] = fmt;
    return c->picPipe[freeSlot];
}

// The glow pyramid for a W x H frame: level 0 at half size, halving until
// the short side would drop under 8 pixels (at most kGlowMax levels).
bool ensureGlow(Ctx* c, int W, int H)
{
    if (c->glowW == W && c->glowH == H && c->glowLevels > 0) return true;
    for (int i = 0; i < Ctx::kGlowMax; ++i) {
        if (c->glowDown[i]) WV_RELEASE(c->glowDown[i]);
        if (c->glowUp[i]) WV_RELEASE(c->glowUp[i]);
    }
    c->glowLevels = 0;
    int w = std::max(1, W / 2), h = std::max(1, H / 2);
    for (int i = 0; i < Ctx::kGlowMax && std::min(w, h) >= 8; ++i) {
        MTLTextureDescriptor* td = [MTLTextureDescriptor
            texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA16Float
                                         width:(NSUInteger)w height:(NSUInteger)h mipmapped:NO];
        td.usage = MTLTextureUsageShaderRead | MTLTextureUsageShaderWrite;
        td.storageMode = MTLStorageModePrivate;
        c->glowDown[i] = [c->dev newTextureWithDescriptor:td];
        c->glowUp[i] = [c->dev newTextureWithDescriptor:td];
        if (!c->glowDown[i] || !c->glowUp[i]) return false;
        c->glowLevels = i + 1;
        w = std::max(1, w / 2); h = std::max(1, h / 2);
    }
    c->glowW = W; c->glowH = H;
    return c->glowLevels > 0;
}

static bool uploadLook(Ctx* c, const LookTable& t)
{
    if (c->lutTex && c->lutVersion == t.version && c->lutSize == t.size) return true;
    const int n = t.size;
    if (!c->lutTex || c->lutSize != n) {
        if (c->lutTex) WV_RELEASE(c->lutTex);
        MTLTextureDescriptor* d = [MTLTextureDescriptor new];
        d.textureType = MTLTextureType3D;
        d.pixelFormat = MTLPixelFormatRGBA32Float;
        d.width = d.height = d.depth = (NSUInteger)n;
        d.usage = MTLTextureUsageShaderRead;
        d.storageMode = c->dev.hasUnifiedMemory ? MTLStorageModeShared : MTLStorageModeManaged;
        c->lutTex = [c->dev newTextureWithDescriptor:d];
#if !__has_feature(objc_arc)
        [d release];
#endif
        if (!c->lutTex) return false;
        c->lutSize = n;
    }
    std::vector<float> rgba((size_t)n * n * n * 4);
    for (size_t i = 0, j = 0; i < (size_t)n * n * n; ++i) {
        rgba[j++] = t.rgb[i * 3]; rgba[j++] = t.rgb[i * 3 + 1]; rgba[j++] = t.rgb[i * 3 + 2]; rgba[j++] = 1.f;
    }
    // Every earlier user of the table has completed: present() and
    // renderPicture() wait for their command buffer before returning.
    [c->lutTex replaceRegion:MTLRegionMake3D(0, 0, 0, (NSUInteger)n, (NSUInteger)n, (NSUInteger)n)
                 mipmapLevel:0 slice:0 withBytes:rgba.data()
                 bytesPerRow:(NSUInteger)n * 4 * sizeof(float)
               bytesPerImage:(NSUInteger)n * n * 4 * sizeof(float)];
    c->lutVersion = t.version;
    return true;
}

// Encode the whole picture pass: the glow pyramid (if any) and the one
// fragment pass into `rp`'s colour attachment.
static void encodePicture(Ctx* c, id<MTLCommandBuffer> cb, id<MTLTexture> src, int sw, int sh,
                          MTLRenderPassDescriptor* rp, int dw, int dh, const PictureSettings& s,
                          id<MTLRenderPipelineState> pipe)
{
    const bool glow = s.glow > 0.001f && ensureGlow(c, sw, sh);
    if (glow) {
        GlowU u = { 1.f / sw, 1.f / sh, 0.5f, 0.4f };       // soft threshold: highlights, not mids
        dispatch2D(cb, c->picPrefilter, src, c->glowDown[0], nil, u);
        for (int i = 1; i < c->glowLevels; ++i) {
            id<MTLTexture> prev = c->glowDown[i - 1];
            GlowU d = { 1.f / prev.width, 1.f / prev.height, 0, 0 };
            dispatch2D(cb, c->picDown, prev, c->glowDown[i], nil, d);
        }
        int L = c->glowLevels;
        id<MTLBlitCommandEncoder> bl = [cb blitCommandEncoder];
        [bl copyFromTexture:c->glowDown[L - 1] toTexture:c->glowUp[L - 1]];
        [bl endEncoding];
        for (int i = L - 2; i >= 0; --i) {
            id<MTLTexture> coarse = c->glowUp[i + 1];
            GlowU d = { 1.f / coarse.width, 1.f / coarse.height, 0, 0 };
            dispatch2D(cb, c->picUp, coarse, c->glowDown[i], c->glowUp[i], d);
        }
    }
    const bool look = s.hasLook() && uploadLook(c, *s.look);
    id<MTLTexture> ovTex = nil;
    if (s.hdr && s.overflow && s.overflow->w == sw && s.overflow->h == sh)
        ovTex = fbTexture(c, *s.overflow);

    PicU u = {};
    u.srcW = (float)sw; u.srcH = (float)sh; u.dstW = (float)dw; u.dstH = (float)dh;
    u.glow = s.glow; u.lookStrength = std::clamp(s.lookStrength, 0.f, 1.f);
    u.lookSize = look ? (float)c->lutSize : 1.f;
    u.vignette = std::clamp(s.vignette, 0.f, 1.f);
    u.scanlines = std::clamp(s.scanlines, 0.f, 1.f);
    u.grain = std::clamp(s.grain, 0.f, 1.f);
    u.bass = std::clamp(s.bass, 0.f, 1.f);
    u.beat = std::clamp(s.beat, 0.f, 1.f);
    u.frame = (float)(s.frame & 0xffff);
    u.pointScale = std::max(1.f, s.pointScale);
    u.sharp = s.sharpScaling ? 1.f : 0.f;
    u.dither = s.dither ? 1.f : 0.f;
    u.hasGlow = glow ? 1.f : 0.f;
    u.hasLook = look ? 1.f : 0.f;
    u.beatReactive = s.beatReactive ? 1.f : 0.f;
    u.glowLevels = (float)(glow ? c->glowLevels : 1);
    // HDR: the peak in display-encoded units (sRGB curve of the linear
    // headroom), capped at 4x white so highlights glow without glaring.
    const float headroom = s.linearOutput ? std::clamp(s.headroom, 1.f, 4.f) : 1.f;
    u.hdr = s.hdr ? 1.f : 0.f;
    u.peak = headroom <= 1.f ? 1.f : 1.055f * std::pow(headroom, 1.f / 2.4f) - 0.055f;
    u.linearOut = s.linearOutput ? 1.f : 0.f;
    u.hasOverflow = ovTex ? 1.f : 0.f;

    id<MTLRenderCommandEncoder> e = [cb renderCommandEncoderWithDescriptor:rp];
    [e setRenderPipelineState:pipe];
    [e setFragmentTexture:src atIndex:0];
    [e setFragmentTexture:(glow ? c->glowUp[0] : c->dummy2D) atIndex:1];
    [e setFragmentTexture:(look ? c->lutTex : c->dummy3D) atIndex:2];
    [e setFragmentTexture:(ovTex ? ovTex : c->dummy2D) atIndex:3];
    [e setFragmentBytes:&u length:sizeof(u) atIndex:0];
    [e drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
    [e endEncoding];
}

bool present(const Framebuffer& fb, void* layerPtr, const PictureSettings& picture)
{
    Ctx* c = ctx();
    if (!c || !layerPtr || fb.w == 0 || fb.h == 0) return false;
    CAMetalLayer* layer = (__bridge CAMetalLayer*)layerPtr;
    auto nowMs = [] { return (double)clock_gettime_nsec_np(CLOCK_MONOTONIC_RAW) * 1e-6; };

    @autoreleasepool {
        id<MTLRenderPipelineState> pipe = picturePipeline(c, layer.pixelFormat);
        if (!pipe) return false;

        double t0 = nowMs();
        id<MTLTexture> R = fbTexture(c, fb);       // zero-copy view of the frame
        if (!R) return false;
        double t1 = nowMs();
        gPresentUploadMs = t1 - t0;

        id<CAMetalDrawable> drawable = [layer nextDrawable];
        double t2 = nowMs();
        gPresentDrawableMs = t2 - t1;
        if (!drawable) return true;                 // drawable pool exhausted: drop this frame

        MTLRenderPassDescriptor* rp = [MTLRenderPassDescriptor renderPassDescriptor];
        rp.colorAttachments[0].texture = drawable.texture;
        rp.colorAttachments[0].loadAction = MTLLoadActionDontCare;
        rp.colorAttachments[0].storeAction = MTLStoreActionStore;

        // The pass joins whatever the frame's effects left pending, so a
        // GPU-only stack is a single command buffer from first kernel to
        // screen. The frame's memory is the CPU's and gets rewritten next
        // tick, so this is where the frame's work is finally waited for.
        id<MTLCommandBuffer> cb = c->cb();
        encodePicture(c, cb, R, fb.w, fb.h, rp,
                      (int)drawable.texture.width, (int)drawable.texture.height, picture, pipe);
        [cb presentDrawable:drawable];
        double t3 = nowMs();
        c->flush();
        gPresentGpuMs = nowMs() - t3;
    }
    return true;
}

// Offscreen picture pass into `fmt`, read back row-flipped so row 0 is the
// framebuffer's row 0. BGRA8 → rgba8 bytes; RGBA32Float → floats.
static bool renderPictureTo(const Framebuffer& fb, const PictureSettings& picture, int outW, int outH,
                            MTLPixelFormat fmt, std::vector<uint8_t>* bytes, std::vector<float>* floats)
{
    Ctx* c = ctx();
    if (!c || c->broken || fb.w == 0 || fb.h == 0 || outW <= 0 || outH <= 0) return false;
    bool ok = false;
    @autoreleasepool {
        id<MTLRenderPipelineState> pipe = picturePipeline(c, fmt);
        if (!pipe) return false;
        id<MTLTexture> R = fbTexture(c, fb);
        if (!R) return false;
        const bool isFloat = fmt == MTLPixelFormatRGBA32Float;
        const NSUInteger px = isFloat ? 16 : 4;
        MTLTextureDescriptor* td = [MTLTextureDescriptor
            texture2DDescriptorWithPixelFormat:fmt width:(NSUInteger)outW height:(NSUInteger)outH mipmapped:NO];
        td.usage = MTLTextureUsageRenderTarget;
        td.storageMode = MTLStorageModePrivate;
        id<MTLTexture> target = [c->dev newTextureWithDescriptor:td];
        const NSUInteger rowBytes = (NSUInteger)outW * px;
        id<MTLBuffer> readback = [c->dev newBufferWithLength:rowBytes * (NSUInteger)outH
                                                     options:MTLResourceStorageModeShared];
        if (target && readback) {
            MTLRenderPassDescriptor* rp = [MTLRenderPassDescriptor renderPassDescriptor];
            rp.colorAttachments[0].texture = target;
            rp.colorAttachments[0].loadAction = MTLLoadActionDontCare;
            rp.colorAttachments[0].storeAction = MTLStoreActionStore;
            id<MTLCommandBuffer> cb = c->cb();
            encodePicture(c, cb, R, fb.w, fb.h, rp, outW, outH, picture, pipe);
            id<MTLBlitCommandEncoder> b = [cb blitCommandEncoder];
            [b copyFromTexture:target sourceSlice:0 sourceLevel:0
                  sourceOrigin:MTLOriginMake(0, 0, 0)
                    sourceSize:MTLSizeMake((NSUInteger)outW, (NSUInteger)outH, 1)
                      toBuffer:readback destinationOffset:0
             destinationBytesPerRow:rowBytes destinationBytesPerImage:rowBytes * (NSUInteger)outH];
            [b endEncoding];
            c->flush();
            // The render target's row 0 is the top of the image, where the
            // framebuffer keeps its last row: flip so row 0 matches fb row 0.
            const uint8_t* p = (const uint8_t*)readback.contents;
            if (isFloat && floats) {
                floats->resize((size_t)outW * outH * 4);
                for (int y = 0; y < outH; ++y)
                    std::memcpy(floats->data() + (size_t)y * outW * 4,
                                p + (size_t)(outH - 1 - y) * rowBytes, rowBytes);
                ok = true;
            } else if (!isFloat && bytes) {
                bytes->resize((size_t)outW * outH * 4);
                for (int y = 0; y < outH; ++y) {
                    const uint8_t* srow = p + (size_t)(outH - 1 - y) * rowBytes;
                    uint8_t* drow = bytes->data() + (size_t)y * rowBytes;
                    for (int x = 0; x < outW; ++x) {
                        drow[x * 4 + 0] = srow[x * 4 + 2];
                        drow[x * 4 + 1] = srow[x * 4 + 1];
                        drow[x * 4 + 2] = srow[x * 4 + 0];
                        drow[x * 4 + 3] = 255;
                    }
                }
                ok = true;
            }
        }
        if (target) WV_RELEASE(target);
        if (readback) WV_RELEASE(readback);
    }
    return ok;
}

bool renderPicture(const Framebuffer& fb, const PictureSettings& picture,
                   int outW, int outH, std::vector<uint8_t>& rgba)
{
    PictureSettings s = picture;
    s.linearOutput = false;                     // an 8-bit image: the SDR path
    return renderPictureTo(fb, s, outW, outH, MTLPixelFormatBGRA8Unorm, &rgba, nullptr);
}

bool renderPictureLinear(const Framebuffer& fb, const PictureSettings& picture,
                         int outW, int outH, std::vector<float>& rgba)
{
    PictureSettings s = picture;
    s.linearOutput = true;                      // what an extended-range layer receives
    return renderPictureTo(fb, s, outW, outH, MTLPixelFormatRGBA32Float, nullptr, &rgba);
}

}} // namespace viz::gpu
