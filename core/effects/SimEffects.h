//
// SimEffects.h — simulations that run on the GPU: flow-field particles,
// stable fluids and Gray-Scott reaction-diffusion (gpu::flowParticles,
// gpu::fluid, gpu::reactionDiffusion). Each works out its audio-driven
// inputs here (forces, splats, seeds) and keeps a smaller CPU version of the
// same simulation for machines without a GPU backend, so presets built on
// them still move there.
//
#pragma once
#include "Effect.h"
#include "GpuFx.h"
#include <cstdint>
#include <vector>

namespace viz {

// Hundreds of thousands of particles riding the curl of animated noise,
// drawn as motion-blurred streaks. Bass pushes them outward, a hit (the
// trigger) bursts them from the centre, treble shakes them.
class FlowParticlesEffect : public Effect {
public:
    ~FlowParticlesEffect() override;
    const char* name() const override { return "Flow Particles"; }
    bool isGpu() const override;
    void render(Framebuffer& cur, const Framebuffer& prev,
                const VizFrame& a, const EffectContext& ctx) override;

    float count      = 150000.f;
    float speed      = 0.45f;   // flow speed (frame heights / s)
    float scale      = 1.6f;    // flow detail (swirls per frame height)
    float follow     = 3.f;     // how quickly particles take the flow
    float drag       = 0.6f;
    float bassPush   = 0.8f;
    float burst      = 0.9f;
    float turbulence = 0.3f;    // treble shake
    float lifetime   = 4.f;
    float width      = 1.f;
    float brightness = 1.f;
    float hue        = 0.55f;
    float hueSpread  = 0.2f;
    float hueSpeed   = 0.f;

    std::vector<Param> params() override {
        return {{"count", 10000.f, 400000.f, &count},
                {"speed", 0.05f, 2.f, &speed},
                {"scale", 0.3f, 6.f, &scale},
                {"follow", 0.2f, 10.f, &follow},
                {"drag", 0.f, 4.f, &drag},
                {"bass_push", 0.f, 3.f, &bassPush},
                {"burst", 0.f, 3.f, &burst},
                {"trigger", 0.f, 3.f, &trigger},
                {"turbulence", 0.f, 2.f, &turbulence},
                {"lifetime", 0.5f, 10.f, &lifetime},
                {"width", 0.5f, 4.f, &width},
                {"brightness", 0.f, 4.f, &brightness},
                {"hue", 0.f, 1.f, &hue},
                {"hue_spread", 0.f, 1.f, &hueSpread},
                {"hue_speed", 0.f, 0.5f, &hueSpeed}};
    }

private:
    gpu::SimState* _gpu = nullptr;
    double _time = 0.0;
    float _seed = 0.f;
    bool _started = false;
    // CPU version: a few thousand of the same particles.
    struct P { float x, y, vx, vy, px, py, life, age, tone; };
    std::vector<P> _cpu;
    uint32_t _rng = 0x9E3779B9u;
    float rnd();
    void renderCpu(Framebuffer& cur, const gpu::ParticleParams& p);
};

// Coloured dye in a swirling fluid: emitters orbit the centre and stir it
// with the music (lows, mids, highs each drive one), a hit bursts outward.
class FluidEffect : public Effect {
public:
    ~FluidEffect() override;
    const char* name() const override { return "Fluid"; }
    bool isGpu() const override;
    void render(Framebuffer& cur, const Framebuffer& prev,
                const VizFrame& a, const EffectContext& ctx) override;

    float scale     = 0.25f;    // grid size / frame size
    float velKeep   = 0.35f;    // velocity kept per second
    float dyeKeep   = 0.45f;    // dye kept per second
    float vorticity = 25.f;
    float force     = 1.f;
    float dye       = 1.f;
    float gain      = 1.f;
    float mode      = 0.f;      // 0 add to the frame, 1 replace it
    float emitters  = 3.f;
    float radius    = 0.06f;    // splat size (frame heights)
    float hue       = 0.58f;
    float hueSpread = 0.35f;
    float hueSpeed  = 0.02f;
    float orbit     = 0.15f;    // emitter orbit speed (turns / s)
    float layout    = 0.f;      // 0: emitters orbit the centre; 1: they wander the whole frame

    std::vector<Param> params() override {
        return {{"scale", 0.1f, 0.5f, &scale},
                {"vel_keep", 0.05f, 1.f, &velKeep},
                {"dye_keep", 0.05f, 1.f, &dyeKeep},
                {"vorticity", 0.f, 60.f, &vorticity},
                {"force", 0.f, 3.f, &force},
                {"dye", 0.f, 3.f, &dye},
                {"gain", 0.f, 3.f, &gain},
                {"mode", 0.f, 1.f, &mode},
                {"emitters", 1.f, 6.f, &emitters},
                {"radius", 0.01f, 0.3f, &radius},
                {"trigger", 0.f, 3.f, &trigger},
                {"hue", 0.f, 1.f, &hue},
                {"hue_spread", 0.f, 1.f, &hueSpread},
                {"hue_speed", 0.f, 0.5f, &hueSpeed},
                {"orbit", 0.f, 1.f, &orbit},
                {"layout", 0.f, 1.f, &layout}};
    }

    // The splats for one frame (shared by the GPU and CPU versions).
    std::vector<gpu::FluidSplat> splatsFor(const VizFrame& a, const EffectContext& ctx, float aspect);

private:
    gpu::SimState* _gpu = nullptr;
    double _time = 0.0;
    uint32_t _hits = 0;
    // CPU version: the same solver on a coarse grid.
    int _gw = 0, _gh = 0;
    std::vector<float> _vx, _vy, _r, _g, _b, _p, _div, _curl, _tmp[5];
    void renderCpu(Framebuffer& cur, const gpu::FluidParams& p);
};

// Gray-Scott reaction-diffusion: spots, stripes and coral that grow and
// divide; the music leans the recipe between them, hits plant new seeds.
class ReactionDiffusionEffect : public Effect {
public:
    ~ReactionDiffusionEffect() override;
    const char* name() const override { return "Reaction Diffusion"; }
    bool isGpu() const override;
    void render(Framebuffer& cur, const Framebuffer& prev,
                const VizFrame& a, const EffectContext& ctx) override;

    float scale    = 0.5f;      // grid size / frame size
    float feed     = 0.037f;
    float kill     = 0.06f;
    float speed    = 12.f;      // steps per frame
    float audio    = 0.6f;      // how far the music leans the recipe
    float seedSize = 0.03f;     // frame heights
    float hue      = 0.08f;
    float mixAmt   = 1.f;
    float emboss   = 0.8f;
    float pulse    = 1.f;       // how hard the music hits the picture

    std::vector<Param> params() override {
        return {{"scale", 0.2f, 1.f, &scale},
                {"feed", 0.01f, 0.1f, &feed},
                {"kill", 0.03f, 0.075f, &kill},
                {"speed", 1.f, 32.f, &speed},
                {"audio", 0.f, 1.f, &audio},
                {"seed_size", 0.005f, 0.1f, &seedSize},
                {"trigger", 0.f, 3.f, &trigger},
                {"hue", 0.f, 1.f, &hue},
                {"mix", 0.f, 1.f, &mixAmt},
                {"emboss", 0.f, 2.f, &emboss},
                {"pulse", 0.f, 2.f, &pulse}};
    }

private:
    gpu::SimState* _gpu = nullptr;
    bool _started = false;
    uint32_t _rng = 0x2545F491u;
    float _feedNow = 0.f, _killNow = 0.f;
    float _hit = 0.f;                               // envelope of the last hit
    float rnd();
    // CPU version: a coarser grid.
    int _gw = 0, _gh = 0;
    std::vector<float> _u, _v, _u2, _v2;
    void renderCpu(Framebuffer& cur, const gpu::RDParams& p, bool reset);
};

// Gradient noise, the same function the GPU flow uses (-1..1).
float simNoise(float x, float y, float z);

} // namespace viz
