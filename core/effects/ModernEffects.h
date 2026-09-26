//
// ModernEffects.h — CPU-side effects original to this project (not AVS
// ports): phosphor trails and a real particle system.
//
#pragma once
#include "Effect.h"
#include <cstdint>

namespace viz {

// Phosphor persistence: mixes the previous frame's FINAL output back in.
// mode 0 = crossfade (soft motion blur), mode 1 = max-decay (CRT phosphor —
// bright content lingers and fades without smearing dark over bright).
class TrailsEffect : public Effect {
public:
    const char* name() const override { return "Trails"; }
    bool usesPrev() const override { return true; }
    void render(Framebuffer& cur, const Framebuffer& prev,
                const VizFrame& a, const EffectContext& ctx) override;

    float persistence = 0.85f;   // how much of the previous frame survives
    float mode        = 1.f;     // 0 crossfade, 1 phosphor max
    float beatFlash   = 0.f;     // temporarily lower persistence on beat

    std::vector<Param> params() override {
        return {{"persistence", 0.f, 0.98f, &persistence},
                {"mode", 0.f, 1.f, &mode},
                {"beat_flash", 0.f, 1.f, &beatFlash},
                {"trigger", 0.f, 3.f, &trigger}};
    }

private:
    double _flash = 0;
};

// Particle system: emitter at center (or scripted via gravity toward/away),
// velocity + drag + gravity physics, beat bursts, additive gaussian sprites
// colored along a hue ramp. Sparse work — CPU is plenty.
class ParticleSystemEffect : public Effect {
public:
    const char* name() const override { return "Particles"; }
    void render(Framebuffer& cur, const Framebuffer& prev,
                const VizFrame& a, const EffectContext& ctx) override;

    float maxCount  = 600.f;
    float emitRate  = 120.f;   // particles/second
    float beatBurst = 150.f;   // extra particles per beat
    float speed     = 0.45f;   // initial speed (half-heights/second)
    float speedBass = 0.8f;    // extra speed from bass energy
    float gravity   = 0.15f;   // downward pull (negative = rise)
    float drag      = 0.5f;    // velocity damping per second
    float size      = 3.5f;    // sprite radius in pixels
    float hue       = 0.58f;   // base hue
    float hueSpread = 0.25f;   // per-particle hue variation
    float lifetime  = 2.2f;    // seconds

    std::vector<Param> params() override {
        return {{"max_count", 32.f, 2000.f, &maxCount},
                {"emit_rate", 0.f, 1000.f, &emitRate},
                {"beat_burst", 0.f, 600.f, &beatBurst},
                {"trigger", 0.f, 3.f, &trigger},
                {"speed", 0.05f, 2.f, &speed},
                {"speed_bass", 0.f, 3.f, &speedBass},
                {"gravity", -1.f, 1.f, &gravity},
                {"drag", 0.f, 3.f, &drag},
                {"size", 1.f, 12.f, &size},
                {"hue", 0.f, 1.f, &hue},
                {"hue_spread", 0.f, 1.f, &hueSpread},
                {"lifetime", 0.3f, 6.f, &lifetime}};
    }

private:
    struct P { float x, y, vx, vy, life, maxLife, hue; };
    std::vector<P> _ps;
    double _emitCarry = 0;
    uint32_t _rng = 0x1234567u;
    float frand();               // 0..1
};

// Phosphor vectorscope: every sample since the previous frame drawn as one
// continuous beam, the way an analog scope's electron beam traces it. The
// beam leaves more light where it moves slowly, so steady shapes glow and
// fast transients stay faint. mode 0: stereo (a goniometer: mono is a
// vertical line, width spreads it sideways). mode 1: phase (the signal
// against itself a moment earlier, so even mono draws loops and knots).
class VectorscopeEffect : public Effect {
public:
    const char* name() const override { return "Vectorscope"; }
    void render(Framebuffer& cur, const Framebuffer& prev,
                const VizFrame& a, const EffectContext& ctx) override;

    float mode     = 1.f;      // 0 stereo, 1 phase
    float delay    = 96.f;     // phase mode: samples between x and y
    float size     = 0.8f;     // fraction of the short side
    float gain     = 1.f;      // beam brightness
    float hue      = 0.36f;    // 0..1 (0.36: green phosphor)
    float hueSpeed = 0.f;      // hue drift per second
    float width    = 1.f;      // beam width (points)
    float normalize= 1.f;      // >0.5: follow the level, so quiet music fills the scope too

    std::vector<Param> params() override {
        return {{"mode", 0.f, 1.f, &mode},
                {"delay", 1.f, 200.f, &delay},
                {"size", 0.1f, 1.f, &size},
                {"gain", 0.f, 3.f, &gain},
                {"hue", 0.f, 1.f, &hue},
                {"hue_speed", 0.f, 0.5f, &hueSpeed},
                {"width", 0.5f, 4.f, &width},
                {"normalize", 0.f, 1.f, &normalize}};
    }

private:
    float _peakX = 0.05f, _peakY = 0.05f; // followed RMS of each axis
    float _lpU = 0.f, _lpV = 0.f;         // the beam's low-pass state
    float _hist[256] = {};       // phase mode: the tail of the previous frame
    std::vector<float> _xs, _ys;  // this frame's beam
    bool _haveLast = false;
    float _lx = 0.f, _ly = 0.f;  // where the beam was at the end of the previous frame
};

} // namespace viz
