//
// TransGeo.h — geometric transform effects, porting classic AVS trans/ effects.
//
// Algorithms adapted from the open-sourced AVS codebase (BSD-3, Nullsoft):
// each class notes the e_<name>.cpp it was ported from. Pixel math is adapted
// from packed 0-255 BGR ints to our float RGBA 0..1 Framebuffer API.
//
#pragma once
#include "Effect.h"

namespace viz {

// Our take on AVS Mirror (e_mirror.cpp, BSD-3). Copies one half of the frame
// over the other: left→right, right→left, top→bottom, bottom→top, or a full
// quadrant mirror. Optional on-beat random re-pick of the mirror directions,
// with a smooth cross-blend transition instead of AVS's 16-step integer ramp.
class MirrorEffect : public Effect {
public:
    const char* name() const override { return "Mirror"; }
    void render(Framebuffer& cur, const Framebuffer& prev,
                const VizFrame& a, const EffectContext& ctx) override;

    float mode         = 1.f;   // 0=off 1=l→r 2=r→l 3=t→b 4=b→t 5=quadrant (truncated)
    float onBeatRandom = 0.f;   // >0.5 = pick random directions on beat
    float transition   = 0.1f;  // blend step per frame toward target (0 = instant)

    std::vector<Param> params() override {
        return {{"mode", 0.f, 5.f, &mode},
                {"on_beat_random", 0.f, 1.f, &onBeatRandom},
                {"transition", 0.f, 1.f, &transition}};
    }

private:
    // per-direction mirror weights 0..1: [0]=l→r [1]=r→l [2]=t→b [3]=b→t
    float _cur[4] = {0, 0, 0, 0};
    float _tgt[4] = {0, 0, 0, 0};
    uint32_t _rng = 0x1234abcdu;
};

// Our take on AVS Mosaic (e_mosaic.cpp, BSD-3). Pixelates the frame into an
// NxN grid of blocks (each block filled from a pixel near its center, as the
// original's fixed-point stepper did). On beat the block count jumps to a
// second value and walks back linearly over a set number of frames.
class MosaicEffect : public Effect {
public:
    const char* name() const override { return "Mosaic"; }
    void render(Framebuffer& cur, const Framebuffer& prev,
                const VizFrame& a, const EffectContext& ctx) override;

    float size       = 50.f;  // blocks across (100 = passthrough, like AVS)
    float onBeat     = 0.f;   // >0.5 = enable on-beat size jump
    float onBeatSize = 10.f;  // block count on beat
    float duration   = 15.f;  // frames to decay back to `size`
    float blend      = 0.f;   // 0=replace 1=additive 2=50/50 (truncated)

    std::vector<Param> params() override {
        return {{"size", 2.f, 100.f, &size},
                {"on_beat", 0.f, 1.f, &onBeat},
                {"on_beat_size", 2.f, 100.f, &onBeatSize},
                {"duration", 1.f, 100.f, &duration},
                {"blend", 0.f, 2.f, &blend}};
    }

private:
    float _curSize  = 50.f;
    int   _cooldown = 0;
    std::vector<float> _scratch;
};

// Our take on AVS Scatter (e_scatter.cpp, BSD-3). Each pixel is fetched from a
// small random nearby offset (the original's 512-entry "fudgetable" of ±3px
// displacements). The top and bottom 4 rows are copied untouched, exactly as
// the original did to keep the offsets in-bounds. `amount` (our addition)
// scales the probability that a pixel scatters at all.
class ScatterEffect : public Effect {
public:
    const char* name() const override { return "Scatter"; }
    void render(Framebuffer& cur, const Framebuffer& prev,
                const VizFrame& a, const EffectContext& ctx) override;

    float amount = 1.f;   // 0..1 probability a pixel is displaced

    std::vector<Param> params() override {
        return {{"amount", 0.f, 1.f, &amount}};
    }

private:
    int _fudgetable[512] = {};
    int _ftw = 0;         // width the table was built for
    uint32_t _rng = 0x8badf00du;
    std::vector<float> _scratch;
};

// Our take on AVS Grain (e_grain.cpp, BSD-3). Film grain over non-black
// pixels: each grainy pixel is the source color scaled by a random byte;
// non-grainy pixels go black (in replace mode) exactly like the original.
// Static mode uses a per-pixel noise field regenerated on resize; temporal
// mode redraws the noise every frame.
class GrainEffect : public Effect {
public:
    const char* name() const override { return "Grain"; }
    void render(Framebuffer& cur, const Framebuffer& prev,
                const VizFrame& a, const EffectContext& ctx) override;

    float amount      = 0.5f;  // fraction of pixels grained
    float staticGrain = 0.f;   // >0.5 = static per-pixel noise field
    float blend       = 2.f;   // 0=replace 1=additive 2=50/50 (truncated)
    float onBeatBoost = 0.f;   // extra amount added on beat frames (our addition)

    std::vector<Param> params() override {
        return {{"amount", 0.f, 1.f, &amount},
                {"static", 0.f, 1.f, &staticGrain},
                {"blend", 0.f, 2.f, &blend},
                {"on_beat_boost", 0.f, 1.f, &onBeatBoost}};
    }

private:
    std::vector<float> _depth;  // (noise, threshold) pairs, per pixel
    int _dw = 0, _dh = 0;
    uint32_t _rng = 0xdeadbeefu;
};

// Our take on AVS Interleave (e_interleave.cpp, BSD-3). Draws bands of a solid
// color interleaved with the image: every other `y`-pixel-tall row band is
// filled; within unfilled rows, every other `x`-pixel-wide column band is
// filled. On beat the strides jump to alternate values and ease back at a
// duration-controlled rate (the original's xy_lerp recurrence).
class InterleaveEffect : public Effect {
public:
    const char* name() const override { return "Interleave"; }
    void render(Framebuffer& cur, const Framebuffer& prev,
                const VizFrame& a, const EffectContext& ctx) override;

    float x = 1.f, y = 1.f;            // band strides in pixels (0 = off)
    float colR = 0.f, colG = 0.f, colB = 0.f;
    float blend    = 0.f;              // 0=replace 1=additive 2=50/50 (truncated)
    float onBeat   = 0.f;              // >0.5 = jump strides on beat
    float onBeatX  = 8.f, onBeatY = 8.f;
    float duration = 32.f;             // 0..64, higher = slower ease back

    std::vector<Param> params() override {
        return {{"x", 0.f, 64.f, &x},
                {"y", 0.f, 64.f, &y},
                {"color_r", 0.f, 1.f, &colR},
                {"color_g", 0.f, 1.f, &colG},
                {"color_b", 0.f, 1.f, &colB},
                {"blend", 0.f, 2.f, &blend},
                {"on_beat", 0.f, 1.f, &onBeat},
                {"on_beat_x", 0.f, 64.f, &onBeatX},
                {"on_beat_y", 0.f, 64.f, &onBeatY},
                {"duration", 0.f, 64.f, &duration}};
    }

private:
    double _curX = 1.0, _curY = 1.0;
};

// Our take on AVS Interferences (e_interferences.cpp, BSD-3). N ghost copies
// of the frame, offset on a rotating circle, summed additively (optionally one
// RGB channel per layer when layers is a multiple of 3). A beat restarts a
// sin()-shaped envelope that sweeps distance/alpha/rotation from their on-beat
// values back to base.
class InterferencesEffect : public Effect {
public:
    const char* name() const override { return "Interferences"; }
    void render(Framebuffer& cur, const Framebuffer& prev,
                const VizFrame& a, const EffectContext& ctx) override;

    float layers      = 2.f;    // number of ghost copies, 0..8 (truncated)
    float distance    = 10.f;   // offset radius in pixels
    float alpha       = 0.5f;   // per-layer brightness
    float rotation    = 2.f;    // rotation step per frame (-32..32, 255 = full turn)
    float blend       = 0.f;    // 0=replace 1=additive 2=50/50 (truncated)
    float onBeat      = 1.f;    // >0.5 = beat sweeps the on-beat values
    float onBeatDistance = 32.f;
    float onBeatAlpha    = 0.75f;
    float onBeatRotation = 8.f;
    float onBeatSpeed    = 0.2f; // envelope advance per frame (0..pi total sweep)
    float separateRGB    = 0.f;  // >0.5 and layers%3==0: one channel per layer

    std::vector<Param> params() override {
        return {{"layers", 0.f, 8.f, &layers},
                {"distance", 1.f, 64.f, &distance},
                {"alpha", 0.f, 1.f, &alpha},
                {"rotation", -32.f, 32.f, &rotation},
                {"blend", 0.f, 2.f, &blend},
                {"on_beat", 0.f, 1.f, &onBeat},
                {"on_beat_distance", 1.f, 64.f, &onBeatDistance},
                {"on_beat_alpha", 0.f, 1.f, &onBeatAlpha},
                {"on_beat_rotation", -32.f, 32.f, &onBeatRotation},
                {"on_beat_speed", 0.01f, 1.28f, &onBeatSpeed},
                {"separate_rgb", 0.f, 1.f, &separateRGB}};
    }

private:
    float _curRotation = 0.f;    // 0..255 units, like the original
    float _fadeout     = 3.1415926535f; // envelope phase, rests at pi
    std::vector<float> _scratch;
};

// Our take on AVS Roto Blitter (e_rotoblitter.cpp, BSD-3). Zoom+rotate blit
// with tiled (wrapping) source, sampling the previous frame like FeedbackWarp.
// On beat the rotation direction can reverse (easing through zero at a
// controllable speed) and the zoom can kick to a second value that eases back.
class RotoBlitterEffect : public Effect {
public:
    const char* name() const override { return "Roto Blitter"; }
    bool isGpu() const override;       // Metal when available, CPU loop otherwise
    void render(Framebuffer& cur, const Framebuffer& prev,
                const VizFrame& a, const EffectContext& ctx) override;

    float zoom         = 1.0f;  // source-step scale: >1 tiles smaller copies, <1 zooms in
    float rotate       = 4.f;   // degrees per frame
    float blend        = 0.f;   // >0.5 = 50/50 blend with existing frame
    float onBeatReverse = 0.f;  // >0.5 = reverse spin direction on beat
    float reverseSpeed  = 2.f;  // higher = slower direction ease (original 0..8)
    float onBeatZoomEnable = 0.f;
    float onBeatZoom       = 1.3f;

    std::vector<Param> params() override {
        return {{"zoom", 0.25f, 4.f, &zoom},
                {"rotate", -32.f, 32.f, &rotate},
                {"blend", 0.f, 1.f, &blend},
                {"on_beat_reverse", 0.f, 1.f, &onBeatReverse},
                {"reverse_speed", 0.f, 8.f, &reverseSpeed},
                {"on_beat_zoom_enable", 0.f, 1.f, &onBeatZoomEnable},
                {"on_beat_zoom", 0.25f, 4.f, &onBeatZoom}};
    }

private:
    float _curZoom  = 1.0f;
    float _curRot   = 1.0f;   // eases between +1 and -1 (direction blend)
    int   _dir      = 1;
    Framebuffer _snap;        // snapshot of cur (AVS transforms the current frame)
};

// Our take on AVS Add Borders (e_addborders.cpp, BSD-3; original APE by
// Goebish). Paints a solid border frame of a given percent size. Our addition:
// an optional on-beat pulse that widens the border and decays back.
class AddBordersEffect : public Effect {
public:
    const char* name() const override { return "Add Borders"; }
    void render(Framebuffer& cur, const Framebuffer& prev,
                const VizFrame& a, const EffectContext& ctx) override;

    float size = 4.f;                  // percent of w/h (1..50, like the original)
    float colR = 1.f, colG = 1.f, colB = 1.f;
    float onBeatPulse = 0.f;           // extra percent added on beat
    float pulseDecay  = 0.85f;         // per-frame decay of the pulse

    std::vector<Param> params() override {
        return {{"size", 1.f, 50.f, &size},
                {"color_r", 0.f, 1.f, &colR},
                {"color_g", 0.f, 1.f, &colG},
                {"color_b", 0.f, 1.f, &colB},
                {"on_beat_pulse", 0.f, 25.f, &onBeatPulse},
                {"pulse_decay", 0.5f, 0.99f, &pulseDecay}};
    }

private:
    float _pulse = 0.f;
};

// Our take on the AVS Movement built-in (e_movement.cpp / e_movement.h,
// BSD-3). The original evaluated tiny scripts over a source-lookup table; we
// reimplement the classic preset formulas as native polar/cartesian math with
// bilinear sampling and the original's wrap option. Modes (truncated index):
//   0 Slight Fuzzify        1 Shift Rotate Left   2 Big Swirl Out
//   3 Medium Swirl          4 Sunburster          5 Swirl To Center
//   6 Bubbling Outward      7 Tunneling           8 Bleedin'
class MovementEffect : public Effect {
public:
    const char* name() const override { return "Movement"; }
    bool isGpu() const override;       // Metal when available, CPU loop otherwise
    void render(Framebuffer& cur, const Framebuffer& prev,
                const VizFrame& a, const EffectContext& ctx) override;

    float mode  = 2.f;   // preset index 0..8 (truncated)
    float wrap  = 0.f;   // >0.5 = source coordinates wrap (tile) instead of clamp
    float blend = 0.f;   // >0.5 = 50/50 blend transformed with original

    std::vector<Param> params() override {
        return {{"mode", 0.f, 8.f, &mode},
                {"wrap", 0.f, 1.f, &wrap},
                {"blend", 0.f, 1.f, &blend}};
    }

private:
    Framebuffer _scratch;
    uint32_t _rng = 0xcafef00du;
};

} // namespace viz
