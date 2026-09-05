//
// BuiltinEffects.h — first clean-room effects, porting standard AVS ideas.
//
#pragma once
#include "Effect.h"

namespace viz {

// Feedback warp: our take on AVS "Movement" / "Dynamic Movement". Each frame it
// resamples the PREVIOUS frame through a zoom+rotate transform and fades it,
// producing trails / a tunnel. Zoom pulses with the bass; a beat kicks it.
class FeedbackWarp : public Effect {
public:
    const char* name() const override { return "Feedback Warp"; }
    bool isGpu() const override;       // Metal when available, CPU loop otherwise
    bool usesPrev() const override { return true; }
    void render(Framebuffer& cur, const Framebuffer& prev,
                const VizFrame& a, const EffectContext& ctx) override;

    float zoomBase   = 1.015f; // >1 zooms in (tunnel), <1 drifts outward
    float zoomBass   = 0.060f; // extra zoom from bass
    float spin       = 0.010f; // radians/frame
    float spinTreble = 0.020f; // extra spin from treble
    float beatKick   = 0.030f; // extra zoom on beat frames
    float decay      = 0.955f; // trail persistence (<1)

    std::vector<Param> params() override {
        return {{"zoom", 0.90f, 1.10f, &zoomBase},
                {"zoom_bass", 0.f, 0.15f, &zoomBass},
                {"spin", -0.08f, 0.08f, &spin},
                {"spin_treble", 0.f, 0.08f, &spinTreble},
                {"beat_kick", 0.f, 0.10f, &beatKick},
                {"decay", 0.75f, 1.0f, &decay}};
    }
};

// Scope: our take on AVS "Superscope" / oscilloscope + spectrum glow. Draws the
// waveform as a bright additive line (hue cycles) plus a spectrum-driven glow,
// on top of the warped feedback — so the scope leaves flowing trails.
class ScopeEffect : public Effect {
public:
    const char* name() const override { return "Scope"; }
    void render(Framebuffer& cur, const Framebuffer& prev,
                const VizFrame& a, const EffectContext& ctx) override;

    float amp  = 0.34f;        // waveform amplitude (fraction of height)
    float gain = 0.9f;         // additive brightness

    std::vector<Param> params() override {
        return {{"amp", 0.05f, 0.6f, &amp},
                {"gain", 0.2f, 1.5f, &gain}};
    }
};

// Blur: our take on the AVS Blur built-in — a separable box blur applied to the
// current frame. In a feedback stack it turns hard scope lines into soft glow.
class BlurEffect : public Effect {
public:
    const char* name() const override { return "Blur"; }
    bool isGpu() const override;       // Metal when available, CPU loop otherwise
    void render(Framebuffer& cur, const Framebuffer& prev,
                const VizFrame& a, const EffectContext& ctx) override;

    float radius = 1.f;        // box radius in pixels (1 = 3x3); truncated to int
    float mix    = 1.0f;       // 0..1 blend between sharp and blurred

    std::vector<Param> params() override {
        return {{"radius", 0.f, 4.f, &radius},
                {"mix", 0.f, 1.f, &mix}};
    }

private:
    std::vector<float> _tmp;   // scratch, sized lazily
};

// Color Map: our take on the classic Color Map APE — replaces each pixel's color
// with a gradient palette looked up by luminance. Beat advances to the next
// palette (AVS's beat-cycling mode).
class ColorMapEffect : public Effect {
public:
    ColorMapEffect();
    const char* name() const override { return "Color Map"; }
    bool isGpu() const override;       // Metal when available, CPU loop otherwise
    void render(Framebuffer& cur, const Framebuffer& prev,
                const VizFrame& a, const EffectContext& ctx) override;

    float cycleOnBeat = 1.f;   // >0.5 = advance palette on beat
    float palette     = 0.f;   // current palette index (truncated)

    std::vector<Param> params() override {
        return {{"palette", 0.f, 3.f, &palette},
                {"cycle_on_beat", 0.f, 1.f, &cycleOnBeat}};
    }

private:
    static constexpr int kLutSize = 256;
    struct Palette { float lut[kLutSize][3]; };
    std::vector<Palette> _palettes;
};

// Starfield: our take on the AVS Starfield built-in — stars fly radially out of
// the center (classic warp-speed field); speed rides the bass, brightness grows
// as stars approach. Drawn additively so it composes with feedback trails.
class StarfieldEffect : public Effect {
public:
    StarfieldEffect();
    const char* name() const override { return "Starfield"; }
    void render(Framebuffer& cur, const Framebuffer& prev,
                const VizFrame& a, const EffectContext& ctx) override;

    float speed      = 0.35f;  // base z-speed per second
    float speedBass  = 1.2f;   // extra speed from bass
    float brightness = 0.9f;

    std::vector<Param> params() override {
        return {{"speed", 0.05f, 1.5f, &speed},
                {"speed_bass", 0.f, 3.f, &speedBass},
                {"brightness", 0.2f, 1.5f, &brightness}};
    }

private:
    struct Star { float x, y, z; };  // x,y in [-1,1], z in (0,1]
    std::vector<Star> _stars;
    uint32_t _rng = 0x9d2c5680u;
    float frand();                   // 0..1, cheap xorshift
};

// The original Music-plugin test visual (core/Renderer.mm DrawVisual) as a
// configurable effect: rainbow spectrum bars from the bottom + waveform trace
// across the middle. The plugin still uses the AppKit original; this is the
// same look living inside the effect stack.
class SpectrumBarsEffect : public Effect {
public:
    const char* name() const override { return "Spectrum Bars"; }
    void render(Framebuffer& cur, const Framebuffer& prev,
                const VizFrame& a, const EffectContext& ctx) override;

    float bars      = 64.f;    // bar count
    float barHeight = 0.75f;   // max bar height (fraction of frame)
    float waveAmp   = 0.22f;   // waveform amplitude (fraction of height)
    float hueShift  = 0.f;     // rotate the rainbow (0..1)
    float saturation= 0.85f;
    float background= 0.f;     // >0.5: fill the plugin's dark bg first

    std::vector<Param> params() override {
        return {{"bars", 8.f, 128.f, &bars},
                {"bar_height", 0.1f, 1.f, &barHeight},
                {"wave_amp", 0.f, 0.5f, &waveAmp},
                {"hue_shift", 0.f, 1.f, &hueShift},
                {"saturation", 0.f, 1.f, &saturation},
                {"background", 0.f, 1.f, &background}};
    }
};

} // namespace viz
