//
// TransFilter.h — convolution / water / filter family, ported from classic AVS.
//
// Algorithms adapted from the vis_avs sources (BSD-3-Clause, Copyright 2005
// Nullsoft, Inc. and contributors; Convolution Filter APE copyright 2002 Tom
// Holden). Packed 0-255 BGR integer math is re-expressed as float RGBA 0..1.
//
#pragma once
#include "Effect.h"

namespace viz {

// Our take on AVS Convolution Filter (e_convolution.cpp, BSD-3). The original
// APE JIT-compiled a user-editable kernel with bias/scale/wrap/absolute
// options. We keep the (sum*weights + bias) / scale core (with optional edge
// wrap and per-preset absolute-value, as the original offered) but replace the
// free-form weight grid with a set of classic 5x5 kernel presets, plus a
// strength mix with the unfiltered image.
class ConvolutionEffect : public Effect {
public:
    const char* name() const override { return "Convolution"; }
    bool isGpu() const override;       // Metal when available, CPU loop otherwise
    void render(Framebuffer& cur, const Framebuffer& prev,
                const VizFrame& a, const EffectContext& ctx) override;

    // 0=identity 1=box blur 2=gaussian 3=sharpen 4=edge detect 5=emboss
    float kernelMode = 2.f;
    float strength   = 1.f;    // 0..1 mix filtered vs original
    float bias       = 0.f;    // added after scaling (original's bias/scale)
    float wrap       = 0.f;    // >0.5 wraps at edges instead of clamping

    std::vector<Param> params() override {
        return {{"kernel_mode", 0.f, 5.f, &kernelMode},
                {"strength", 0.f, 1.f, &strength},
                {"bias", -1.f, 1.f, &bias},
                {"wrap", 0.f, 1.f, &wrap}};
    }

private:
    std::vector<float> _src;   // scratch copy of cur, sized lazily
};

// Our take on AVS Multi Filter (e_multifilter.cpp, BSD-3). Modes: the "chrome"
// value-fold (dark and bright both go dark, mids go bright) applied 1/2/3
// times, and the "infinite root / small border convolution" mask (any non-black
// pixel becomes white and smears one pixel up/left — faithfully including the
// original APE's lopsided 2-neighbor convolution). Optional beat toggle
// flip-flops the whole effect on/off, as in the original.
class MultiFilterEffect : public Effect {
public:
    const char* name() const override { return "Multi Filter"; }
    void render(Framebuffer& cur, const Framebuffer& prev,
                const VizFrame& a, const EffectContext& ctx) override;

    // 0=chrome 1=double chrome 2=triple chrome 3=infroot border convolution
    float mode         = 0.f;
    float toggleOnBeat = 0.f;  // >0.5: each beat toggles the effect on/off

    std::vector<Param> params() override {
        return {{"mode", 0.f, 3.f, &mode},
                {"toggle_on_beat", 0.f, 1.f, &toggleOnBeat}};
    }

private:
    bool _toggleState = false;
};

// Our take on AVS Normalise (e_normalise.cpp, BSD-3). Per-frame histogram
// stretch: scans the min and max channel value over the frame and remaps
// linearly so the darkest goes to 0 and the brightest to 1 (skipped when the
// frame already spans the full range; a flat frame goes black, per original).
class NormaliseEffect : public Effect {
public:
    const char* name() const override { return "Normalise"; }
    void render(Framebuffer& cur, const Framebuffer& prev,
                const VizFrame& a, const EffectContext& ctx) override;

    float mix = 1.f;           // 0..1 blend between input and normalised

    std::vector<Param> params() override {
        return {{"mix", 0.f, 1.f, &mix}};
    }
};

// Our take on AVS Water (e_water.cpp, BSD-3). The classic two-buffer wave
// trick applied to color: each pixel becomes (sum of its 4 neighbors in the
// incoming frame) / 2 minus its own value from the frame before (edge pixels
// use the original's 2- and 3-neighbor variants), clamped. Over the feedback
// chain this makes anything drawn ripple outward organically. The original has
// no tunables; neither do we.
class WaterEffect : public Effect {
public:
    const char* name() const override { return "Water"; }
    void render(Framebuffer& cur, const Framebuffer& prev,
                const VizFrame& a, const EffectContext& ctx) override;

private:
    std::vector<float> _last;  // previous input frame (rgb), sized lazily
    PixelVector _out;          // scratch output (swapped into cur)
    int _lw = 0, _lh = 0;
};

// Our take on AVS Water Bump (e_waterbump.cpp, BSD-3). A real height-field
// water sim on two ping-pong buffers with the classic 8-neighbor update and
// fluidity damping; each beat drops a cosine-shaped raindrop (random position
// or a fixed grid position). The height gradient then displaces (refracts) the
// current frame, gradient/8 pixels, like the original.
class WaterBumpEffect : public Effect {
public:
    const char* name() const override { return "Water Bump"; }
    void render(Framebuffer& cur, const Framebuffer& prev,
                const VizFrame& a, const EffectContext& ctx) override;

    float fluidity   = 6.f;    // damping = 1 - 2^-fluidity (original's shift)
    float depth      = 600.f;  // drop depth (original height units)
    float dropRadius = 40.f;   // drop radius, % of the larger dimension
    float random     = 1.f;    // >0.5 random drop position, else fixed grid
    float dropPosX   = 1.f;    // fixed grid 0=1/4 1=center 2=3/4 (truncated)
    float dropPosY   = 1.f;

    std::vector<Param> params() override {
        return {{"fluidity", 2.f, 12.f, &fluidity},
                {"depth", 50.f, 2000.f, &depth},
                {"drop_radius", 5.f, 100.f, &dropRadius},
                {"random", 0.f, 1.f, &random},
                {"drop_pos_x", 0.f, 2.f, &dropPosX},
                {"drop_pos_y", 0.f, 2.f, &dropPosY}};
    }

private:
    std::vector<float> _height[2];  // ping-pong height fields
    std::vector<float> _scratch;    // displaced-frame scratch (rgba)
    int _bw = 0, _bh = 0;
    int _page = 0;
    uint32_t _rng = 0x2545f491u;
    float frand();                  // 0..1, cheap xorshift
    void sineBlob(int x, int y, int radius, float height);
};

// Our take on AVS Bump (e_bump.cpp, BSD-3). Bump-map lighting: the frame's
// max-channel value acts as a height field; the finite-difference slope toward
// a moving light adds brightness (the original's 127-|gradient - light dir|
// product, depth-scaled). The original scripted the light path in EEL; we
// replace it with an orbit around the center (speed + radius params). On-beat
// depth boost with linear decay over a duration mirrors the original's
// onbeat/duration/depth trio; invert and replace/additive/5050 blends kept.
class BumpEffect : public Effect {
public:
    const char* name() const override { return "Bump"; }
    void render(Framebuffer& cur, const Framebuffer& prev,
                const VizFrame& a, const EffectContext& ctx) override;

    float depth       = 30.f;  // 0..100, as the original slider
    float onBeat      = 1.f;   // >0.5 enables beat depth boost
    float beatDepth   = 100.f; // depth jumped to on beat
    float beatDur     = 15.f;  // frames to decay back
    float orbitSpeed  = 0.5f;  // light revolutions per second
    float orbitRadius = 0.25f; // orbit radius, fraction of min dimension
    float invert      = 0.f;   // >0.5 inverts the height field
    float blendMode   = 0.f;   // 0=replace 1=additive 2=50/50 (truncated)

    std::vector<Param> params() override {
        return {{"depth", 0.f, 100.f, &depth},
                {"onbeat", 0.f, 1.f, &onBeat},
                {"beat_depth", 0.f, 100.f, &beatDepth},
                {"beat_dur", 1.f, 60.f, &beatDur},
                {"orbit_speed", -2.f, 2.f, &orbitSpeed},
                {"orbit_radius", 0.f, 0.5f, &orbitRadius},
                {"invert", 0.f, 1.f, &invert},
                {"blend", 0.f, 2.f, &blendMode}};
    }

private:
    PixelVector _out;          // scratch output (rgba, swapped into cur)
    float _curDepth   = 30.f;  // live depth incl. beat decay
    int   _beatFrames = 0;     // frames left in on-beat fadeout
};

} // namespace viz
