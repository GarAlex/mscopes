//
// GpuFx.h — the Metal backend, first slice.
//
// Design: the effect ABI stays CPU-shaped (Framebuffer in/out). GPU effects
// call these helpers, which lazily bring up a Metal device + runtime-compiled
// kernels, upload the framebuffer to an rgba32Float texture, run the passes,
// and read the result back. On Apple silicon (unified memory) the transfers
// are cheap; batching consecutive GPU passes on-texture is a later
// optimization once more of the stack lives here.
//
// Header is C++-clean (no ObjC) so any effect can include it.
//
#pragma once
#include "Framebuffer.h"
#include "Picture.h"
#include <cstdint>
#include <string>
#include <vector>
#include <string>

namespace viz { namespace gpu {

// True if a Metal device is present and the kernel library compiled.
// All ops below silently no-op when unavailable. Set MSCOPES_NO_GPU=1 in
// the environment to force the CPU paths (A/B testing, debugging).
bool available();

// The backend's MTLDevice (as an opaque pointer; nullptr when unavailable).
// A CAMetalLayer that present() draws into must use this same device.
void* metalDevice();

// --- presentation --------------------------------------------------------
// Draw fb into the next drawable of a CAMetalLayer (passed as void* so this
// header stays C++-clean) through the picture pass (Picture.h): scaling to
// the drawable (sharp or bilinear), glow, colour look, vignette, grain,
// scanlines and dither, all in one fragment pass plus a small glow pyramid.
// Only the drawable is written — fb, the frame a preset feeds forward, is
// read and never modified. Returns false when Metal is unavailable (caller
// falls back to the CPU blit).
bool present(const Framebuffer& fb, void* caMetalLayer, const PictureSettings& picture);
inline bool present(const Framebuffer& fb, void* caMetalLayer)
{
    return present(fb, caMetalLayer, PictureSettings());
}

// The same picture pass into an offscreen outW x outH image, read back as
// RGBA8 (row 0 = the framebuffer's row 0). For tests and vizrender.
// Returns false without a GPU.
bool renderPicture(const Framebuffer& fb, const PictureSettings& picture,
                   int outW, int outH, std::vector<uint8_t>& rgba);

// The same into linear-light floats, as an extended-range layer receives
// them (values above 1 = brighter than SDR white). For tests.
bool renderPictureLinear(const Framebuffer& fb, const PictureSettings& picture,
                         int outW, int outH, std::vector<float>& rgba);

// Profiling: milliseconds the last present() spent binding the frame (always
// ~0 now), waiting for a drawable, and waiting for the GPU.
void lastPresentTimes(double* bindMs, double* drawableWaitMs, double* gpuWaitMs);

// --- CPU/GPU coherence -----------------------------------------------------
// Zero-copy backend: a Framebuffer's page-aligned pixels are wrapped by an
// MTLBuffer-backed linear texture, so ops read and write the CPU's memory
// directly. Ops are DEFERRED: they encode into one pending command buffer
// and only flush() executes it and waits. Hosts call the three sync
// functions below at exactly the points where the CPU is about to read or
// write pixels (before a CPU effect, at frame end, before a snapshot), and
// present() flushes too; they all just flush. Anything else that touches
// a framebuffer's memory outside those points must call flush() first.
void flush();
void syncToCpu(Framebuffer& fb);              // CPU will write fb: flush
void syncToCpuForRead(const Framebuffer& fb); // CPU will read fb: flush
void invalidateResident(const void* fbAddr);  // fb is being cleared/destroyed: flush

// Bloom: luminance-threshold prefilter → multi-scale pyramid (depth set by
// radius, in source pixels) → frame + glow * intensity through a soft
// rolloff. With an overflow buffer (HDR highlights) the light above white
// the rolloff removed is added there; the frame is the same either way.
void bloom(Framebuffer& fb, float threshold, float radius, float intensity,
           Framebuffer* overflow = nullptr);

// N-fold mirrored kaleidoscope around the center. angle animates the spin,
// zoom scales the sampled source (1 = none).
void kaleidoscope(Framebuffer& fb, int segments, float angle, float zoom);

// Chromatic aberration: red/blue sampled ±offset along `angle` (radians),
// green stays. dx/dy given in pixels via amount*cos/sin(angle).
void rgbSplit(Framebuffer& fb, float amountPx, float angle);

// Up to 4 expanding radial displacement waves (radii in unit half-height,
// negative = inactive), gaussian band of `width`, displacing by `strength`.
void shockwave(Framebuffer& fb, const float radii[4], float width, float strength);

// Digital corruption: horizontal band shifts, RGB tearing, momentary
// posterize. amount 0..1, seed reseeds the pattern, blockPx = band height.
void glitch(Framebuffer& fb, float amount, float seed, float blockPx, float tearPx);

// Anamorphic streaks: threshold + long horizontal blur tail, tinted, added.
void streaks(Framebuffer& fb, float threshold, float length, float intensity,
             float tintR, float tintG, float tintB);

// Lens distortion: >0 fisheye/barrel, <0 pincushion (aspect-true).
void lens(Framebuffer& fb, float strength);

// Zoom blur toward the center; amount 0..1, taps = quality.
void radialBlur(Framebuffer& fb, float amount, int taps);

// Sobel edge glow, tinted; keepSource 0 = edges only, 1 = added over frame.
void edges(Framebuffer& fb, float glow, float keepSource,
           float r, float g, float b);

// Two-color luminance grade (shadow → highlight), mixed by mixAmt.
void duotone(Framebuffer& fb, const float shadow[3], const float highlight[3], float mixAmt);

// Heat shimmer: animated value-noise displacement field (amount in pixels).
void shimmer(Framebuffer& fb, float amountPx, float scale, float t);

// CRT simulation: barrel curvature, scanlines, phosphor triad mask, corners.
void crt(Framebuffer& fb, float curvature, float scanlines, float mask, float corner);

// Filmic-ish tone map: exposure rolloff (1-exp), gamma, saturation scale.
// With an overflow buffer the exposed light above white is added there.
void toneMap(Framebuffer& fb, float exposure, float gamma, float saturation,
             Framebuffer* overflow = nullptr);

// Darken toward the frame edge: fade from `inner` to `outer` radius
// (aspect-true, 1 = half-height) by `strength` (0..1).
void vignette(Framebuffer& fb, float inner, float outer, float strength);

// Trails against the previous frame (same size): mode 0 crossfade, 1
// phosphor max, 2 fade (the frame becomes the previous one times
// persistence, as Clear Screen + trails would leave it, in one pass).
void trails(Framebuffer& fb, const Framebuffer& prev, float persistence, int mode);

// --- classic ports ---------------------------------------------------------
// GPU versions of the CPU effects that dominated profiles (Movement, Dynamic
// Movement, Convolution, Roto Blitter, Blur, Feedback Warp: 84% of frame time
// over a 298-preset community sample). Same math as the CPU loops, which stay
// as the fallback; sampling matches Framebuffer::sample (clamp) / sampleWrap.

// Movement built-in modes 0..8 (MovementEffect); `seed` varies the fuzzify
// jitter per frame.
void movement(Framebuffer& fb, int mode, bool wrap, bool blend, int seed);

// Roto Blitter: resample fb through the affine (ca, sa: cos/sin with zoom
// folded in) about the center, tiling source; optional 50/50 blend.
void rotoBlit(Framebuffer& fb, float ca, float sa, bool blend);

// Feedback warp: fb = sample(prev, affine) * decay (prev is uploaded).
void feedbackWarp(Framebuffer& fb, const Framebuffer& prev, float ca, float sa, float decay);

// Separable box blur of integer radius, mixed with the original by `mix`.
void boxBlur(Framebuffer& fb, int radius, float mix);

// Dynamic Movement's per-pixel stage: bilinearly interpolate the GX*GY grid of
// (srcX, srcY, alpha) triples across the frame and resample.
void gridWarp(Framebuffer& fb, const float* grid, int GX, int GY, bool wrap, bool blend);

// 5x5 convolution: (sum k*src) * invScale + bias, optional |v|, mixed by `mix`.
void convolve5(Framebuffer& fb, const float k[25], float invScale, float bias,
               bool absolute, bool wrap, float mix);

// Dynamic Shift's per-pixel stage: offset the frame by (sx, sy) pixels, black
// where uncovered; optional alpha blend with the original.
void shift(Framebuffer& fb, float sx, float sy, bool blend, float alpha, bool bilinear);

// Dynamic Distance Modifier's per-pixel stage: radial remap through `n`
// per-radius scale factors (index = integer distance from the center).
void distanceModifier(Framebuffer& fb, const float* table, int n, bool blend, bool bilinear);

// Per-pixel color transforms (the TransColor family + Color Map). `op`
// selects the formula and `params` its inputs — the table is documented at
// fx_colorop in GpuFx.mm; `lut` (256 rgb triplets) is for Color Map.
void colorOp(Framebuffer& fb, int op, const float* params, int nParams,
             const float* lut = nullptr, int lutFloats = 0);

// Effect List blend: dest = blend(dest, src) per AVS mode id (EffectList.h).
// Returns false for modes it doesn't handle (0 Ignore, 12 Buffer) or a size
// mismatch — the caller then runs its CPU loop.
bool blend(Framebuffer& dest, const Framebuffer& src, int mode, float adjustable);

// Run a caller-supplied compute kernel (entry point "px_main", src at
// texture(0), dst at texture(1), float uniforms at buffer(0)) over the
// framebuffer. Pipelines are cached by source text, so per-frame calls with
// an unchanged script cost no recompilation. Returns false and sets *err on
// compile failure (also cached, so a broken script doesn't retry every frame).
bool runCustomKernel(Framebuffer& fb, const std::string& source,
                     const float* uniforms, int nUniforms, std::string* err);

// --- simulations -------------------------------------------------------------
// Persistent GPU state (particle buffers, fluid and reaction-diffusion grids)
// behind an opaque handle the effect owns and frees with simRelease(). Each
// call advances one frame and composites the result into fb. They return
// false when the GPU can't run them (no device, a pipeline failed): the
// effect then runs its CPU version. The effects work out the audio-driven
// inputs (splats, seeds, forces) on the CPU, so both versions share them.
struct SimState;
void simRelease(SimState*& s);

// Flow-field particles. Positions are in height units (y -1..1 down the
// frame, x -aspect..aspect); the flow is the curl of animated noise, so it
// never converges. Each particle is drawn as a motion-blurred streak from
// where it was to where it is, additive, light spread over its length.
struct ParticleParams {
    int   count = 150000;
    float dt = 1.f / 60.f, time = 0.f;
    float speed = 0.5f;         // flow speed, height units / s
    float scale = 1.5f;         // noise frequency (features per height)
    float follow = 3.f;         // how fast velocity turns toward the flow (1/s)
    float drag = 0.5f;          // 1/s
    float push = 0.f;           // outward push this frame (bass), height units / s^2
    float burst = 0.f;          // outward impulse this frame (a hit), height units / s
    float turbulence = 0.f;     // random kick, height units / s
    float lifetime = 4.f;       // seconds (each particle 0.5..1.5x)
    float width = 1.f;          // streak width, pixels
    float brightness = 1.f;
    float hue = 0.55f, hueSpread = 0.2f;
    float seed = 0.f;           // changes the respawn pattern
    bool  reset = false;        // scatter every particle afresh
};
bool flowParticles(SimState*& s, Framebuffer& fb, const ParticleParams& p, Framebuffer* overflow);

// Stable fluids (semi-Lagrangian advection, vorticity confinement, Jacobi
// pressure) on a grid `scale` times the frame, carrying coloured dye that is
// added to (mode 0) or replaces (mode 1) the frame.
struct FluidSplat {
    float x, y;                 // 0..1 across and down the frame
    float vx, vy;               // velocity added, frame heights / s
    float r, g, b;              // dye added
    float radius;               // fraction of the frame height
};
struct FluidParams {
    float scale = 0.25f;        // grid size / frame size
    float dt = 1.f / 60.f;
    float velKeep = 0.99f;      // velocity kept per second (0..1)
    float dyeKeep = 0.6f;       // dye kept per second (0..1)
    float vorticity = 20.f;
    int   iterations = 20;      // pressure solve
    float gain = 1.f;           // dye brightness in the frame
    int   mode = 0;             // 0 add, 1 replace
    const FluidSplat* splats = nullptr;
    int   nSplats = 0;
    bool  reset = false;
};
bool fluid(SimState*& s, Framebuffer& fb, const FluidParams& p, Framebuffer* overflow);

// Gray-Scott reaction-diffusion on a grid `scale` times the frame. U and V
// start as U=1, V=0; seeds set V in discs. The result is coloured from
// colorA (no V) to colorB (full V), shaded by its slope, and mixed over the
// frame by `mix`.
struct RDSeed { float x, y, radius; };   // 0..1 across / down, radius in frame heights
struct RDParams {
    float scale = 0.5f;
    float feed = 0.037f, kill = 0.06f;
    float du = 1.f, dv = 0.5f;
    int   iterations = 12;
    const RDSeed* seeds = nullptr;
    int   nSeeds = 0;
    float colorA[3] = {0.02f, 0.02f, 0.05f};
    float colorB[3] = {0.9f, 0.6f, 0.3f};
    float mix = 1.f;
    float emboss = 0.6f;
    bool  reset = false;
};
bool reactionDiffusion(SimState*& s, Framebuffer& fb, const RDParams& p);

}} // namespace viz::gpu
