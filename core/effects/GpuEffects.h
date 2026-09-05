//
// GpuEffects.h — modern effects on the Metal backend (core/gpu/GpuFx).
//
// These are original to this project (not AVS ports): they exploit the float
// pipeline in ways classic 8-bit AVS couldn't. Each is a no-op when Metal is
// unavailable (viz::gpu::available()).
//
#pragma once
#include "Effect.h"
#include "ScriptedTrans.h"

namespace viz {

// HDR-style bloom: bright regions above `threshold` glow outward with a
// gaussian falloff and a soft (exponential) highlight rolloff.
class BloomEffect : public Effect {
public:
    const char* name() const override { return "Bloom"; }
    bool isGpu() const override { return true; }
    void render(Framebuffer& cur, const Framebuffer& prev,
                const VizFrame& a, const EffectContext& ctx) override;

    float threshold = 0.55f;
    float radius    = 14.f;    // pixels
    float intensity = 1.1f;
    float bassBoost = 0.6f;    // extra intensity from bass energy

    std::vector<Param> params() override {
        return {{"threshold", 0.f, 1.f, &threshold},
                {"radius", 1.f, 64.f, &radius},
                {"intensity", 0.f, 4.f, &intensity},
                {"bass_boost", 0.f, 3.f, &bassBoost}};
    }
};

// N-fold mirrored kaleidoscope with continuous spin.
class KaleidoscopeEffect : public Effect {
public:
    const char* name() const override { return "Kaleidoscope"; }
    bool isGpu() const override { return true; }
    void render(Framebuffer& cur, const Framebuffer& prev,
                const VizFrame& a, const EffectContext& ctx) override;

    float segments  = 6.f;     // 2..16 mirror folds
    float spin      = 0.15f;   // radians/sec
    float spinBeat  = 0.35f;   // extra kick on beat (decays)
    float zoom      = 1.f;

    std::vector<Param> params() override {
        return {{"segments", 2.f, 16.f, &segments},
                {"spin", -2.f, 2.f, &spin},
                {"spin_beat", 0.f, 2.f, &spinBeat},
                {"zoom", 0.5f, 2.f, &zoom}};
    }

private:
    double _angle = 0, _kick = 0, _lastTime = -1;
};

// Chromatic aberration: red/blue fringe along a rotatable axis, optionally
// pumped by the beat.
class RgbSplitEffect : public Effect {
public:
    const char* name() const override { return "RGB Split"; }
    bool isGpu() const override { return true; }
    void render(Framebuffer& cur, const Framebuffer& prev,
                const VizFrame& a, const EffectContext& ctx) override;

    float amount    = 2.5f;    // pixels
    float angle     = 0.f;     // radians
    float beatPump  = 6.f;     // extra pixels on beat (decays)

    std::vector<Param> params() override {
        return {{"amount", 0.f, 24.f, &amount},
                {"angle", 0.f, 6.2832f, &angle},
                {"beat_pump", 0.f, 24.f, &beatPump}};
    }

private:
    double _pump = 0, _lastTime = -1;
};

// Filmic exposure/gamma/saturation — lets bright stacks breathe instead of
// clipping (the float pipeline's tone control).
class ToneMapEffect : public Effect {
public:
    const char* name() const override { return "Tone Map"; }
    bool isGpu() const override { return true; }
    void render(Framebuffer& cur, const Framebuffer& prev,
                const VizFrame& a, const EffectContext& ctx) override;

    float exposure   = 1.4f;
    float gamma      = 1.0f;
    float saturation = 1.15f;

    std::vector<Param> params() override {
        return {{"exposure", 0.25f, 4.f, &exposure},
                {"gamma", 0.5f, 2.5f, &gamma},
                {"saturation", 0.f, 2.f, &saturation}};
    }
};

// Darkened frame edges; focuses the eye center-frame in fullscreen.
class VignetteEffect : public Effect {
public:
    const char* name() const override { return "Vignette"; }
    bool isGpu() const override { return true; }
    void render(Framebuffer& cur, const Framebuffer& prev,
                const VizFrame& a, const EffectContext& ctx) override;

    float inner    = 0.55f;    // radius where the fade starts (1 = half-height)
    float outer    = 1.25f;    // radius of full effect
    float strength = 0.65f;

    std::vector<Param> params() override {
        return {{"inner", 0.f, 1.5f, &inner},
                {"outer", 0.2f, 2.f, &outer},
                {"strength", 0.f, 1.f, &strength}};
    }
};

// Beat-triggered expanding shockwaves that physically displace pixels.
class ShockwaveEffect : public Effect {
public:
    const char* name() const override { return "Shockwave"; }
    bool isGpu() const override { return true; }
    void render(Framebuffer& cur, const Framebuffer& prev,
                const VizFrame& a, const EffectContext& ctx) override;

    float speed    = 1.4f;     // expansion, unit half-heights/second
    float width    = 0.08f;    // ring thickness
    float strength = 0.05f;    // displacement amount
    float bassGate = 0.3f;     // only fire on beats with bass above this

    std::vector<Param> params() override {
        return {{"speed", 0.3f, 4.f, &speed},
                {"width", 0.02f, 0.3f, &width},
                {"strength", 0.f, 0.2f, &strength},
                {"bass_gate", 0.f, 1.f, &bassGate}};
    }

private:
    float _radii[4] = {-1, -1, -1, -1};
    double _lastTime = -1;
};

// Beat-gated digital corruption (band shifts, RGB tears, posterize flashes).
class GlitchEffect : public Effect {
public:
    const char* name() const override { return "Glitch"; }
    bool isGpu() const override { return true; }
    void render(Framebuffer& cur, const Framebuffer& prev,
                const VizFrame& a, const EffectContext& ctx) override;

    float intensity = 0.7f;    // glitch amount at full energy
    float decay     = 4.f;     // energy decay rate (per second)
    float constant  = 0.f;     // baseline glitch even without beats
    float blockSize = 18.f;    // band height in pixels
    float tear      = 6.f;     // RGB tear in pixels

    std::vector<Param> params() override {
        return {{"intensity", 0.f, 1.f, &intensity},
                {"decay", 0.5f, 12.f, &decay},
                {"constant", 0.f, 1.f, &constant},
                {"block_size", 4.f, 64.f, &blockSize},
                {"tear", 0.f, 24.f, &tear}};
    }

private:
    double _energy = 0, _lastTime = -1;
    float _seed = 1.f;
};

// Anamorphic streak flare: long tinted horizontal bloom off bright points.
class StreaksEffect : public Effect {
public:
    const char* name() const override { return "Streaks"; }
    bool isGpu() const override { return true; }
    void render(Framebuffer& cur, const Framebuffer& prev,
                const VizFrame& a, const EffectContext& ctx) override;

    float threshold = 0.6f;
    float length    = 40.f;    // streak reach (pixels of final blur sigma)
    float intensity = 0.9f;
    float tintR = 0.55f, tintG = 0.7f, tintB = 1.f;   // classic anamorphic blue

    std::vector<Param> params() override {
        return {{"threshold", 0.f, 1.f, &threshold},
                {"length", 8.f, 63.f, &length},
                {"intensity", 0.f, 3.f, &intensity},
                {"tint_r", 0.f, 1.f, &tintR},
                {"tint_g", 0.f, 1.f, &tintG},
                {"tint_b", 0.f, 1.f, &tintB}};
    }
};

// Lens distortion: fisheye bulge (or pincushion with negative strength),
// optionally pumped by bass for a breathing-glass look.
class LensEffect : public Effect {
public:
    const char* name() const override { return "Lens"; }
    bool isGpu() const override { return true; }
    void render(Framebuffer& cur, const Framebuffer& prev,
                const VizFrame& a, const EffectContext& ctx) override;

    float strength  = 0.35f;   // >0 fisheye, <0 pincushion
    float bassBoost = 0.4f;

    std::vector<Param> params() override {
        return {{"strength", -1.f, 1.f, &strength},
                {"bass_boost", 0.f, 2.f, &bassBoost}};
    }
};

// Zoom blur toward the center — the classic VJ "warp speed" smear, with a
// beat pump that decays.
class RadialBlurEffect : public Effect {
public:
    const char* name() const override { return "Radial Blur"; }
    bool isGpu() const override { return true; }
    void render(Framebuffer& cur, const Framebuffer& prev,
                const VizFrame& a, const EffectContext& ctx) override;

    float amount   = 0.25f;
    float beatPump = 0.5f;     // extra amount on beat (decays)
    float quality  = 16.f;     // sample taps

    std::vector<Param> params() override {
        return {{"amount", 0.f, 1.f, &amount},
                {"beat_pump", 0.f, 1.f, &beatPump},
                {"quality", 4.f, 32.f, &quality}};
    }

private:
    double _pump = 0, _lastTime = -1;
};

// Sobel edge glow: outlines everything in tinted neon. keep_source blends
// the original back under the wireframe.
class NeonEdgesEffect : public Effect {
public:
    const char* name() const override { return "Neon Edges"; }
    bool isGpu() const override { return true; }
    void render(Framebuffer& cur, const Framebuffer& prev,
                const VizFrame& a, const EffectContext& ctx) override;

    float glow       = 2.5f;
    float keepSource = 0.25f;  // 0 = wireframe only, 1 = full source under
    float tintR = 0.3f, tintG = 1.f, tintB = 0.9f;

    std::vector<Param> params() override {
        return {{"glow", 0.f, 8.f, &glow},
                {"keep_source", 0.f, 1.f, &keepSource},
                {"tint_r", 0.f, 1.f, &tintR},
                {"tint_g", 0.f, 1.f, &tintG},
                {"tint_b", 0.f, 1.f, &tintB}};
    }
};

// Two-color luminance grade: shadows → one color, highlights → another.
class DuotoneEffect : public Effect {
public:
    const char* name() const override { return "Duotone"; }
    bool isGpu() const override { return true; }
    void render(Framebuffer& cur, const Framebuffer& prev,
                const VizFrame& a, const EffectContext& ctx) override;

    float shadowR = 0.05f, shadowG = 0.03f, shadowB = 0.25f;   // deep indigo
    float highR = 1.f, highG = 0.45f, highB = 0.25f;           // hot coral
    float mix = 0.85f;

    std::vector<Param> params() override {
        return {{"shadow_r", 0.f, 1.f, &shadowR},
                {"shadow_g", 0.f, 1.f, &shadowG},
                {"shadow_b", 0.f, 1.f, &shadowB},
                {"high_r", 0.f, 1.f, &highR},
                {"high_g", 0.f, 1.f, &highG},
                {"high_b", 0.f, 1.f, &highB},
                {"mix", 0.f, 1.f, &mix}};
    }
};

// Heat shimmer: everything breathes through an animated noise displacement
// field; bass makes the air ripple harder.
class ShimmerEffect : public Effect {
public:
    const char* name() const override { return "Shimmer"; }
    bool isGpu() const override { return true; }
    void render(Framebuffer& cur, const Framebuffer& prev,
                const VizFrame& a, const EffectContext& ctx) override;

    float amount    = 3.f;     // displacement in pixels
    float scale     = 9.f;     // noise field frequency
    float speed     = 0.8f;    // field drift speed
    float bassBoost = 1.5f;    // extra pixels from bass energy

    std::vector<Param> params() override {
        return {{"amount", 0.f, 16.f, &amount},
                {"scale", 2.f, 40.f, &scale},
                {"speed", 0.f, 4.f, &speed},
                {"bass_boost", 0.f, 8.f, &bassBoost}};
    }

private:
    double _t = 0, _lastTime = -1;
};

// CRT display simulation — run the 2001 presets the way they looked in 2001.
class CRTEffect : public Effect {
public:
    const char* name() const override { return "CRT"; }
    bool isGpu() const override { return true; }
    void render(Framebuffer& cur, const Framebuffer& prev,
                const VizFrame& a, const EffectContext& ctx) override;

    float curvature = 0.12f;
    float scanlines = 0.35f;
    float mask      = 0.25f;   // phosphor triad strength
    float corner    = 0.5f;

    std::vector<Param> params() override {
        return {{"curvature", 0.f, 0.4f, &curvature},
                {"scanlines", 0.f, 0.8f, &scanlines},
                {"mask", 0.f, 0.8f, &mask},
                {"corner", 0.f, 1.f, &corner}};
    }
};

// Per-PIXEL scripted effect — the thing classic AVS never had. init/frame/
// beat scripts run on the CPU VM; the `point` script is transpiled EEL→MSL
// and runs per pixel on the GPU. Per-pixel variables: x, y (-1..1), d, r,
// red/green/blue (read AND write), w, h. Everything else the script touches
// (t, bass, mid, treb, b, reg00…, your own frame-script vars) is snapshotted
// after the frame script and fed in as uniforms.
class PixelShaderEffect : public ScriptedEffectBase {
public:
    PixelShaderEffect();
    const char* name() const override { return "Pixel Shader"; }
    bool isGpu() const override { return true; }
    void render(Framebuffer& cur, const Framebuffer& prev,
                const VizFrame& a, const EffectContext& ctx) override;

    // Last transpile/compile error ("" when healthy) — surfaced to editors.
    const std::string& shaderError() const { return _error; }

private:
    void bindVars(eel::VM& vm) override;
    double *_x = nullptr, *_y = nullptr, *_d = nullptr, *_r = nullptr;
    double *_red = nullptr, *_green = nullptr, *_blue = nullptr;
    double *_w = nullptr, *_h = nullptr, *_t = nullptr;

    std::string _msl;                  // transpiled kernel
    std::vector<double*> _uniformSlots;
    std::string _transpiledFrom;       // pixel-script text the MSL came from
    std::string _error;
    uint32_t _frameSeed = 1;
};

} // namespace viz
