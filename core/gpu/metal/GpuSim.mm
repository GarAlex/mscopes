//
// GpuSim.mm — simulations: flow particles, fluid, reaction-diffusion.
// How the Metal backend is split up: GpuMetal.h.
//
#include "GpuMetal.h"

namespace viz { namespace gpu {

// ---------------------------------------------------------------------------
//  Simulations: flow-field particles, stable fluids, reaction-diffusion
// ---------------------------------------------------------------------------
static const char* kSimShaders =
#include "shaders/Sim.msl.inc"
;

struct SimPipes {
    bool tried = false, ok = false;
    id<MTLLibrary> lib = nil;
    id<MTLComputePipelineState> partStep = nil, add = nil, clear = nil,
        flAdvect = nil, flSplat = nil, flCurl = nil, flVort = nil, flDiv = nil, flJacobi = nil, flGrad = nil,
        rdStep = nil, rdSeed = nil, rdComposite = nil;
    id<MTLRenderPipelineState> partDraw = nil;
};
static SimPipes gSim;

static bool ensureSim(Ctx* c)
{
    if (gSim.tried) return gSim.ok;
    gSim.tried = true;
    NSError* err = nil;
    gSim.lib = [c->dev newLibraryWithSource:@(kSimShaders) options:nil error:&err];
    if (!gSim.lib) { NSLog(@"[gpu] simulation shaders failed: %@", err); return false; }
    auto comp = [&](NSString* name) -> id<MTLComputePipelineState> {
        id<MTLFunction> fn = [gSim.lib newFunctionWithName:name];
        id<MTLComputePipelineState> ps = fn ? [c->dev newComputePipelineStateWithFunction:fn error:&err] : nil;
#if !__has_feature(objc_arc)
        [fn release];
#endif
        if (!ps) NSLog(@"[gpu] simulation pipeline %@ failed: %@", name, err);
        return ps;
    };
    gSim.partStep = comp(@"sim_part_step");
    gSim.add = comp(@"sim_add");
    gSim.clear = comp(@"sim_clear");
    gSim.flAdvect = comp(@"sim_fl_advect");
    gSim.flSplat = comp(@"sim_fl_splat");
    gSim.flCurl = comp(@"sim_fl_curl");
    gSim.flVort = comp(@"sim_fl_vort");
    gSim.flDiv = comp(@"sim_fl_div");
    gSim.flJacobi = comp(@"sim_fl_jacobi");
    gSim.flGrad = comp(@"sim_fl_grad");
    gSim.rdStep = comp(@"sim_rd_step");
    gSim.rdSeed = comp(@"sim_rd_seed");
    gSim.rdComposite = comp(@"sim_rd_composite");
    {
        id<MTLFunction> vs = [gSim.lib newFunctionWithName:@"sim_part_vs"];
        id<MTLFunction> fs = [gSim.lib newFunctionWithName:@"sim_part_fs"];
        MTLRenderPipelineDescriptor* d = [MTLRenderPipelineDescriptor new];
        d.vertexFunction = vs;
        d.fragmentFunction = fs;
        MTLRenderPipelineColorAttachmentDescriptor* ca = d.colorAttachments[0];
        ca.pixelFormat = MTLPixelFormatRGBA16Float;
        ca.blendingEnabled = YES;
        ca.rgbBlendOperation = ca.alphaBlendOperation = MTLBlendOperationAdd;
        ca.sourceRGBBlendFactor = ca.sourceAlphaBlendFactor = MTLBlendFactorOne;
        ca.destinationRGBBlendFactor = ca.destinationAlphaBlendFactor = MTLBlendFactorOne;
        gSim.partDraw = (vs && fs) ? [c->dev newRenderPipelineStateWithDescriptor:d error:&err] : nil;
        if (!gSim.partDraw) NSLog(@"[gpu] particle draw pipeline failed: %@", err);
#if !__has_feature(objc_arc)
        [vs release]; [fs release]; [d release];
#endif
    }
    gSim.ok = gSim.partStep && gSim.add && gSim.clear && gSim.flAdvect && gSim.flSplat && gSim.flCurl
           && gSim.flVort && gSim.flDiv && gSim.flJacobi && gSim.flGrad && gSim.rdStep && gSim.rdSeed
           && gSim.rdComposite && gSim.partDraw;
    return gSim.ok;
}

struct SimState {
    int kind = 0;                                   // 1 particles, 2 fluid, 3 reaction-diffusion
    uint32_t frame = 0;
    // particles
    id<MTLBuffer> parts = nil;
    int count = 0;
    id<MTLTexture> light = nil;                     // frame-sized rgba16Float, additive
    int lightW = 0, lightH = 0;
    // grids
    int gw = 0, gh = 0;
    id<MTLTexture> vel[2] = {nil, nil}, dye[2] = {nil, nil}, pr[2] = {nil, nil};
    id<MTLTexture> div = nil, curl = nil;
    id<MTLTexture> rd[2] = {nil, nil};
    int vi = 0, di = 0, ri = 0;
};

void simRelease(SimState*& s)
{
    if (!s) return;
    flush();                                        // pending work may still use the state
    WV_RELEASE(s->parts);
    WV_RELEASE(s->light);
    for (auto& t : s->vel) WV_RELEASE(t);
    for (auto& t : s->dye) WV_RELEASE(t);
    for (auto& t : s->pr) WV_RELEASE(t);
    for (auto& t : s->rd) WV_RELEASE(t);
    WV_RELEASE(s->div);
    WV_RELEASE(s->curl);
    delete s;
    s = nullptr;
}

static id<MTLTexture> simTexture(Ctx* c, int w, int h, MTLPixelFormat fmt, bool renderTarget = false)
{
    MTLTextureDescriptor* td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:fmt
        width:(NSUInteger)w height:(NSUInteger)h mipmapped:NO];
    td.usage = MTLTextureUsageShaderRead | MTLTextureUsageShaderWrite
             | (renderTarget ? MTLTextureUsageRenderTarget : 0);
    td.storageMode = MTLStorageModePrivate;
    return [c->dev newTextureWithDescriptor:td];
}

// One compute dispatch over `out`'s size.
static void simDispatch(id<MTLCommandBuffer> cb, id<MTLComputePipelineState> ps,
                        std::initializer_list<id<MTLTexture>> texs, id<MTLTexture> out,
                        const void* bytes0, size_t len0, const void* bytes1 = nullptr, size_t len1 = 0)
{
    id<MTLComputeCommandEncoder> e = [cb computeCommandEncoder];
    [e setComputePipelineState:ps];
    NSUInteger i = 0;
    for (id<MTLTexture> t : texs) [e setTexture:t atIndex:i++];
    if (bytes0) [e setBytes:bytes0 length:len0 atIndex:0];
    if (bytes1) [e setBytes:bytes1 length:len1 atIndex:1];
    MTLSize tg = MTLSizeMake(16, 16, 1);
    MTLSize grid = MTLSizeMake((out.width + 15) / 16, (out.height + 15) / 16, 1);
    [e dispatchThreadgroups:grid threadsPerThreadgroup:tg];
    [e endEncoding];
}

// Composite `light` (any size, sampled bilinear) into fb: add or replace,
// clamped at white, the excess to the overflow when given.
static void simComposite(Ctx* c, id<MTLCommandBuffer> cb, Framebuffer& fb, id<MTLTexture> R,
                         id<MTLTexture> light, float gain, int mode, Framebuffer* overflow)
{
    id<MTLTexture> OV = overflowTexture(c, fb, overflow);
    id<MTLTexture> A = c->tex[0], B = c->tex[1];
    float u[4] = { gain, OV ? 1.f : 0.f, (float)mode, 0.f };
    simDispatch(cb, gSim.add, {R, light, A, OV ? OV : B}, A, u, sizeof(u));
    id<MTLBlitCommandEncoder> b = [cb blitCommandEncoder];
    const MTLSize sz = MTLSizeMake((NSUInteger)fb.w, (NSUInteger)fb.h, 1);
    [b copyFromTexture:A sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0, 0, 0) sourceSize:sz
             toTexture:R destinationSlice:0 destinationLevel:0 destinationOrigin:MTLOriginMake(0, 0, 0)];
    [b endEncoding];
}

// Mirror of the shader's PartU.
struct PartUHost {
    float aspect, dt, time, speed;
    float scale, follow, drag, push;
    float burst, turbulence, lifetime, width;
    float brightness, hue, hueSpread, seed;
    float W, H; uint32_t count, frame;
    uint32_t reset, pad0, pad1, pad2;
};

bool flowParticles(SimState*& s, Framebuffer& fb, const ParticleParams& p, Framebuffer* overflow)
{
    Ctx* c; id<MTLTexture> R = beginOp(c, fb);
    if (!R || !ensureSim(c)) return false;
    if (s && s->kind != 1) simRelease(s);
    if (!s) { s = new SimState; s->kind = 1; }
    const int count = std::clamp(p.count, 1000, 1000000);
    bool reset = p.reset;
    if (!s->parts || s->count != count) {
        flush();
        WV_RELEASE(s->parts);
        s->parts = [c->dev newBufferWithLength:(NSUInteger)count * 32 options:MTLResourceStorageModePrivate];
        if (!s->parts) return false;
        s->count = count;
        reset = true;
    }
    if (!s->light || s->lightW != fb.w || s->lightH != fb.h) {
        flush();
        WV_RELEASE(s->light);
        s->light = simTexture(c, fb.w, fb.h, MTLPixelFormatRGBA16Float, true);
        if (!s->light) return false;
        s->lightW = fb.w; s->lightH = fb.h;
    }
    @autoreleasepool {
        id<MTLCommandBuffer> cb = c->cb();
        PartUHost u = {};
        u.aspect = (float)fb.w / (float)fb.h; u.dt = p.dt; u.time = p.time; u.speed = p.speed;
        u.scale = p.scale; u.follow = p.follow; u.drag = p.drag; u.push = p.push;
        u.burst = p.burst; u.turbulence = p.turbulence; u.lifetime = std::max(0.2f, p.lifetime); u.width = p.width;
        u.brightness = p.brightness; u.hue = p.hue; u.hueSpread = p.hueSpread; u.seed = p.seed;
        u.W = (float)fb.w; u.H = (float)fb.h; u.count = (uint32_t)count; u.frame = s->frame++;
        u.reset = reset ? 1u : 0u;
        {
            id<MTLComputeCommandEncoder> e = [cb computeCommandEncoder];
            [e setComputePipelineState:gSim.partStep];
            [e setBuffer:s->parts offset:0 atIndex:0];
            [e setBytes:&u length:sizeof(u) atIndex:1];
            [e dispatchThreads:MTLSizeMake((NSUInteger)count, 1, 1) threadsPerThreadgroup:MTLSizeMake(256, 1, 1)];
            [e endEncoding];
        }
        {
            MTLRenderPassDescriptor* rp = [MTLRenderPassDescriptor renderPassDescriptor];
            rp.colorAttachments[0].texture = s->light;
            rp.colorAttachments[0].loadAction = MTLLoadActionClear;
            rp.colorAttachments[0].clearColor = MTLClearColorMake(0, 0, 0, 0);
            rp.colorAttachments[0].storeAction = MTLStoreActionStore;
            id<MTLRenderCommandEncoder> e = [cb renderCommandEncoderWithDescriptor:rp];
            [e setRenderPipelineState:gSim.partDraw];
            [e setVertexBuffer:s->parts offset:0 atIndex:0];
            [e setVertexBytes:&u length:sizeof(u) atIndex:1];
            [e drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:6 instanceCount:(NSUInteger)count];
            [e endEncoding];
        }
        simComposite(c, cb, fb, R, s->light, 1.f, 0, overflow);
        if (++c->pendingOps >= 64) c->flush();
    }
    return true;
}

static bool simGrid(Ctx* c, SimState* s, int gw, int gh, bool fluidGrids)
{
    if (s->gw == gw && s->gh == gh && (fluidGrids ? s->vel[0] != nil : s->rd[0] != nil)) return true;
    flush();
    for (auto& t : s->vel) WV_RELEASE(t);
    for (auto& t : s->dye) WV_RELEASE(t);
    for (auto& t : s->pr) WV_RELEASE(t);
    for (auto& t : s->rd) WV_RELEASE(t);
    WV_RELEASE(s->div);
    WV_RELEASE(s->curl);
    s->gw = gw; s->gh = gh;
    if (fluidGrids) {
        for (int i = 0; i < 2; ++i) {
            s->vel[i] = simTexture(c, gw, gh, MTLPixelFormatRGBA16Float);
            s->dye[i] = simTexture(c, gw, gh, MTLPixelFormatRGBA16Float);
            s->pr[i] = simTexture(c, gw, gh, MTLPixelFormatR32Float);
            if (!s->vel[i] || !s->dye[i] || !s->pr[i]) return false;
        }
        s->div = simTexture(c, gw, gh, MTLPixelFormatR32Float);
        s->curl = simTexture(c, gw, gh, MTLPixelFormatR32Float);
        if (!s->div || !s->curl) return false;
    } else {
        for (int i = 0; i < 2; ++i) {
            s->rd[i] = simTexture(c, gw, gh, MTLPixelFormatRG32Float);
            if (!s->rd[i]) return false;
        }
    }
    return true;
}

bool fluid(SimState*& s, Framebuffer& fb, const FluidParams& p, Framebuffer* overflow)
{
    Ctx* c; id<MTLTexture> R = beginOp(c, fb);
    if (!R || !ensureSim(c)) return false;
    if (s && s->kind != 2) simRelease(s);
    if (!s) { s = new SimState; s->kind = 2; }
    const float sc = std::clamp(p.scale, 0.05f, 1.f);
    const int gw = std::max(16, (int)std::lround(fb.w * sc)), gh = std::max(9, (int)std::lround(fb.h * sc));
    bool reset = p.reset || s->gw != gw || s->gh != gh || !s->vel[0];
    if (!simGrid(c, s, gw, gh, true)) { simRelease(s); return false; }
    @autoreleasepool {
        id<MTLCommandBuffer> cb = c->cb();
        if (reset) {
            float z[4] = {0, 0, 0, 0};
            for (int i = 0; i < 2; ++i) {
                simDispatch(cb, gSim.clear, {s->vel[i]}, s->vel[i], z, sizeof(z));
                simDispatch(cb, gSim.clear, {s->dye[i]}, s->dye[i], z, sizeof(z));
                simDispatch(cb, gSim.clear, {s->pr[i]}, s->pr[i], z, sizeof(z));
            }
        }
        auto flip = [](int& i) { i ^= 1; };
        struct FlUHost { float dt, keep, vort, pad; };
        const float dt = std::clamp(p.dt, 0.f, 0.1f);
        // 1. splats: velocity, then dye
        if (p.nSplats > 0 && p.splats) {
            const int n = std::min(p.nSplats, 32);
            float info[4] = { (float)n, 0.f, 0.f, 0.f };
            simDispatch(cb, gSim.flSplat, {s->vel[s->vi], s->vel[s->vi ^ 1]}, s->vel[s->vi ^ 1],
                        p.splats, sizeof(FluidSplat) * (size_t)n, info, sizeof(info));
            flip(s->vi);
            info[1] = 1.f;
            simDispatch(cb, gSim.flSplat, {s->dye[s->di], s->dye[s->di ^ 1]}, s->dye[s->di ^ 1],
                        p.splats, sizeof(FluidSplat) * (size_t)n, info, sizeof(info));
            flip(s->di);
        }
        // 2. vorticity confinement
        FlUHost u = { dt, 1.f, p.vorticity, 0.f };
        simDispatch(cb, gSim.flCurl, {s->vel[s->vi], s->curl}, s->curl, nullptr, 0);
        simDispatch(cb, gSim.flVort, {s->vel[s->vi], s->curl, s->vel[s->vi ^ 1]}, s->vel[s->vi ^ 1], &u, sizeof(u));
        flip(s->vi);
        // 3. pressure projection
        simDispatch(cb, gSim.flDiv, {s->vel[s->vi], s->div}, s->div, nullptr, 0);
        for (int it = 0; it < std::clamp(p.iterations, 1, 80); ++it) {
            simDispatch(cb, gSim.flJacobi, {s->pr[s->ri], s->div, s->pr[s->ri ^ 1]}, s->pr[s->ri ^ 1], nullptr, 0);
            flip(s->ri);
        }
        simDispatch(cb, gSim.flGrad, {s->pr[s->ri], s->vel[s->vi], s->vel[s->vi ^ 1]}, s->vel[s->vi ^ 1], nullptr, 0);
        flip(s->vi);
        // 4. advection: velocity through itself, then dye
        u.keep = std::pow(std::clamp(p.velKeep, 0.f, 1.f), dt);
        simDispatch(cb, gSim.flAdvect, {s->vel[s->vi], s->vel[s->vi], s->vel[s->vi ^ 1]}, s->vel[s->vi ^ 1], &u, sizeof(u));
        flip(s->vi);
        u.keep = std::pow(std::clamp(p.dyeKeep, 0.f, 1.f), dt);
        simDispatch(cb, gSim.flAdvect, {s->dye[s->di], s->vel[s->vi], s->dye[s->di ^ 1]}, s->dye[s->di ^ 1], &u, sizeof(u));
        flip(s->di);
        // 5. into the frame
        simComposite(c, cb, fb, R, s->dye[s->di], p.gain, p.mode, overflow);
        if (++c->pendingOps >= 64) c->flush();
    }
    return true;
}

struct RDUHost {
    float du, dv, feed, kill;
    float n, emboss, mixAmt, aspect;
    float colA[4], colB[4];
};

bool reactionDiffusion(SimState*& s, Framebuffer& fb, const RDParams& p)
{
    Ctx* c; id<MTLTexture> R = beginOp(c, fb);
    if (!R || !ensureSim(c)) return false;
    if (s && s->kind != 3) simRelease(s);
    if (!s) { s = new SimState; s->kind = 3; }
    const float sc = std::clamp(p.scale, 0.1f, 1.f);
    const int gw = std::max(16, (int)std::lround(fb.w * sc)), gh = std::max(9, (int)std::lround(fb.h * sc));
    bool reset = p.reset || s->gw != gw || s->gh != gh || !s->rd[0];
    if (!simGrid(c, s, gw, gh, false)) { simRelease(s); return false; }
    @autoreleasepool {
        id<MTLCommandBuffer> cb = c->cb();
        RDUHost u = {};
        u.du = p.du; u.dv = p.dv; u.feed = p.feed; u.kill = p.kill;
        u.emboss = p.emboss; u.mixAmt = p.mix; u.aspect = (float)gw / (float)gh;
        for (int k = 0; k < 3; ++k) { u.colA[k] = p.colorA[k]; u.colB[k] = p.colorB[k]; }
        if (reset) {
            float one[4] = {1, 0, 0, 0};
            simDispatch(cb, gSim.clear, {s->rd[s->ri]}, s->rd[s->ri], one, sizeof(one));
        }
        if (p.nSeeds > 0 && p.seeds) {
            const int n = std::min(p.nSeeds, 64);
            std::vector<float> sd((size_t)n * 4);
            for (int k = 0; k < n; ++k) {
                sd[(size_t)k * 4] = p.seeds[k].x; sd[(size_t)k * 4 + 1] = p.seeds[k].y;
                sd[(size_t)k * 4 + 2] = p.seeds[k].radius; sd[(size_t)k * 4 + 3] = 0.f;
            }
            u.n = (float)n;
            simDispatch(cb, gSim.rdSeed, {s->rd[s->ri], s->rd[s->ri ^ 1]}, s->rd[s->ri ^ 1],
                        sd.data(), sd.size() * sizeof(float), &u, sizeof(u));
            s->ri ^= 1;
        }
        for (int it = 0; it < std::clamp(p.iterations, 0, 64); ++it) {
            simDispatch(cb, gSim.rdStep, {s->rd[s->ri], s->rd[s->ri ^ 1]}, s->rd[s->ri ^ 1], &u, sizeof(u));
            s->ri ^= 1;
        }
        id<MTLTexture> A = c->tex[0];
        simDispatch(cb, gSim.rdComposite, {R, s->rd[s->ri], A}, A, &u, sizeof(u));
        id<MTLBlitCommandEncoder> b = [cb blitCommandEncoder];
        [b copyFromTexture:A sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0, 0, 0)
                sourceSize:MTLSizeMake((NSUInteger)fb.w, (NSUInteger)fb.h, 1)
                 toTexture:R destinationSlice:0 destinationLevel:0 destinationOrigin:MTLOriginMake(0, 0, 0)];
        [b endEncoding];
        if (++c->pendingOps >= 64) c->flush();
    }
    return true;
}

}} // namespace viz::gpu
