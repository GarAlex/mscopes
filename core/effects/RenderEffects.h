//
// RenderEffects.h — classic AVS render effects (scopes, dots, 3D points),
// ported from the open-sourced vis_avs sources (BSD-3, Nullsoft). Each class
// notes the e_<name>.cpp it derives from; algorithms follow the originals,
// adapted to float RGBA 0..1 framebuffers and normalized audio
// (waveform -1..1, spectrum 0..1) instead of byte buffers.
//
#pragma once
#include "Effect.h"

namespace viz {

// Our take on AVS "Simple" (e_simple.cpp, BSD-3): the classic combined
// oscilloscope / spectrum analyzer. mode selects osc {lines,dots,solid} or
// analyzer {lines,dots}; position selects top/center/bottom band. Drawn for
// both channels (left/right hues, right offset a few pixels) instead of the
// original's left/right/center channel switch.
class SimpleScopeEffect : public Effect {
public:
    const char* name() const override { return "Simple Scope"; }
    void render(Framebuffer& cur, const Framebuffer& prev,
                const VizFrame& a, const EffectContext& ctx) override;

    float mode     = 0.f;   // 0 osc lines, 1 osc dots, 2 osc solid, 3 spec lines, 4 spec dots
    float position = 1.f;   // 0 top, 1 center, 2 bottom
    float hueLeft  = 0.33f; // channel-0 hue
    float hueRight = 0.66f; // channel-1 hue
    float gain     = 0.8f;  // additive brightness

    std::vector<Param> params() override {
        return {{"mode", 0.f, 4.f, &mode},
                {"position", 0.f, 2.f, &position},
                {"hue_left", 0.f, 1.f, &hueLeft},
                {"hue_right", 0.f, 1.f, &hueRight},
                {"gain", 0.1f, 1.5f, &gain}};
    }
};

// Our take on AVS "Ring" (e_ring.cpp, BSD-3): an 80-segment circular scope
// whose radius is modulated by the waveform (mirrored across the two halves,
// radius = 0.1..1.0 of size like the original). thickness draws extra
// concentric passes; the original's color-list fade is a single hue here.
class RingEffect : public Effect {
public:
    const char* name() const override { return "Ring"; }
    void render(Framebuffer& cur, const Framebuffer& prev,
                const VizFrame& a, const EffectContext& ctx) override;

    float size      = 0.25f; // ring radius as fraction of min(w,h) (orig size/32)
    float thickness = 1.f;   // concentric passes (truncated)
    float hue       = 0.5f;
    float gain      = 0.8f;
    float posX      = 1.f;   // 0 left, 1 center, 2 right (orig HPOS)

    std::vector<Param> params() override {
        return {{"size", 0.05f, 1.f, &size},
                {"thickness", 1.f, 4.f, &thickness},
                {"hue", 0.f, 1.f, &hue},
                {"gain", 0.1f, 1.5f, &gain},
                {"pos_x", 0.f, 2.f, &posX}};
    }
};

// Our take on AVS "Oscilloscope Star" (e_oscilloscopestar.cpp, BSD-3): the
// waveform is drawn along 5 rotating arms, 64 samples each, with the
// perpendicular deflection ramping up toward the arm tips exactly as the
// original's dfactor ramp (1/1024 -> 1/128) does.
class OscStarEffect : public Effect {
public:
    const char* name() const override { return "Oscilloscope Star"; }
    void render(Framebuffer& cur, const Framebuffer& prev,
                const VizFrame& a, const EffectContext& ctx) override;

    float size     = 0.25f; // star radius as fraction of min(w,h)
    float rotSpeed = 8.f;   // orig "rotation": angle += 0.01*rotSpeed per frame
    float hue      = 0.15f;
    float gain     = 0.8f;

    std::vector<Param> params() override {
        return {{"size", 0.05f, 1.f, &size},
                {"rot_speed", -32.f, 32.f, &rotSpeed},
                {"hue", 0.f, 1.f, &hue},
                {"gain", 0.1f, 1.5f, &gain}};
    }

private:
    double _rot = 0.0;
};

// Our take on AVS "Rotating Stars" (e_rotstar.cpp, BSD-3): two 5-point star
// polygons (one per channel) orbiting the center in opposition, sized by a
// spectral peak picked from low bins 3..14 exactly like the original.
class RotStarEffect : public Effect {
public:
    const char* name() const override { return "Rotating Stars"; }
    void render(Framebuffer& cur, const Framebuffer& prev,
                const VizFrame& a, const EffectContext& ctx) override;

    float speed   = 0.1f;  // radians/frame orbit+spin (orig fixed 0.1)
    float hueLeft = 0.0f;
    float hueRight= 0.5f;
    float gain    = 0.8f;

    std::vector<Param> params() override {
        return {{"speed", -0.5f, 0.5f, &speed},
                {"hue_left", 0.f, 1.f, &hueLeft},
                {"hue_right", 0.f, 1.f, &hueRight},
                {"gain", 0.1f, 1.5f, &gain}};
    }

private:
    double _rot = 0.0;
};

// Our take on AVS "Bass Spin" (e_bassspin.cpp, BSD-3): two spinning arms
// (left/right channel), spin velocity integrating the summed low-spectrum
// energy through the original's AGC (a = d*512/(last+30*256)) and velocity
// smoothing (v = 0.7*drive + 0.3*v). mode 0 draws the outline (arm + trail
// lines), mode 1 fills triangles between successive arm positions.
class BassSpinEffect : public Effect {
public:
    const char* name() const override { return "Bass Spin"; }
    void render(Framebuffer& cur, const Framebuffer& prev,
                const VizFrame& a, const EffectContext& ctx) override;

    float mode    = 1.f;   // 0 outline lines, 1 filled triangles
    float hueLeft = 0.98f;
    float hueRight= 0.55f;
    float gain    = 0.8f;

    std::vector<Param> params() override {
        return {{"mode", 0.f, 1.f, &mode},
                {"hue_left", 0.f, 1.f, &hueLeft},
                {"hue_right", 0.f, 1.f, &hueRight},
                {"gain", 0.1f, 1.5f, &gain}};
    }

private:
    float  _lastA = 0.f;
    double _v[2]  = {0.0, 0.0};
    double _rv[2] = {3.14159, 0.0};
    double _dir[2]= {-1.0, 1.0};
    int    _lx[2][2] = {{0,0},{0,0}};   // [point 0/1][channel]
    int    _ly[2][2] = {{0,0},{0,0}};
    bool   _hasLast[2] = {false, false};
};

// Our take on AVS "Dot Grid" (e_dotgrid.cpp, BSD-3): a regular grid of dots
// scrolling with independent x/y velocities (fractional accumulators like the
// original's 8.8 fixed point), blended additively.
class DotGridEffect : public Effect {
public:
    const char* name() const override { return "Dot Grid"; }
    void render(Framebuffer& cur, const Framebuffer& prev,
                const VizFrame& a, const EffectContext& ctx) override;

    float spacing = 8.f;    // pixels between dots (>=2, truncated)
    float speedX  = 0.5f;   // pixels/frame
    float speedY  = 0.25f;  // pixels/frame
    float hue     = 0.6f;
    float gain    = 0.8f;

    std::vector<Param> params() override {
        return {{"spacing", 2.f, 64.f, &spacing},
                {"speed_x", -8.f, 8.f, &speedX},
                {"speed_y", -8.f, 8.f, &speedY},
                {"hue", 0.f, 1.f, &hue},
                {"gain", 0.1f, 1.5f, &gain}};
    }

private:
    float _xp = 0.f, _yp = 0.f;   // scroll accumulators (pixels)
};

// Our take on AVS "Dot Plane" (e_dotplane.cpp, BSD-3): a 64x64 plane of dots
// seen in 3D. Each frame the spectrum feeds a new edge row of heights which
// ripples across the plane (per-dot velocity with the original's 0.15 decay),
// while the whole plane rotates about Y. Colors come from the original's
// 5-stop gradient LUT indexed by height. Full 4x4 matrix pipeline ported.
class DotPlaneEffect : public Effect {
public:
    DotPlaneEffect();
    const char* name() const override { return "Dot Plane"; }
    void render(Framebuffer& cur, const Framebuffer& prev,
                const VizFrame& a, const EffectContext& ctx) override;

    float rotSpeed = 16.f;  // orig -50..50; rotation += rotSpeed/5 deg/frame
    float angle    = -20.f; // camera tilt, degrees (orig -90..91)
    float gain     = 1.0f;

    std::vector<Param> params() override {
        return {{"rot_speed", -50.f, 50.f, &rotSpeed},
                {"angle", -90.f, 91.f, &angle},
                {"gain", 0.1f, 1.5f, &gain}};
    }

private:
    static constexpr int kGrid = 64;
    static constexpr int kMapSize = 64;    // 4 intervals * 16 lerp steps
    float _height[kGrid * kGrid] = {};
    float _delta[kGrid * kGrid]  = {};
    float _color[kGrid * kGrid][3] = {};
    float _map[kMapSize][3];
    double _rotation = 0.0;   // degrees
};

// Our take on AVS "Dot Fountain" (e_dotfountain.cpp, BSD-3): rings of colored
// dots launched upward from the center, launch speed and color driven by the
// waveform (boosted on beat like the original's is_beat*128), pulled back by
// gravity (+0.05/frame) and flung outward, under a rotating tilted camera.
// Same 30-around x 256-generations point pool and 5-stop color LUT.
class DotFountainEffect : public Effect {
public:
    DotFountainEffect();
    const char* name() const override { return "Dot Fountain"; }
    void render(Framebuffer& cur, const Framebuffer& prev,
                const VizFrame& a, const EffectContext& ctx) override;

    float rotSpeed = 16.f;  // orig -50..50; rotation += rotSpeed/5 deg/frame
    float angle    = -20.f; // camera tilt, degrees
    float gain     = 1.0f;

    std::vector<Param> params() override {
        return {{"rot_speed", -50.f, 50.f, &rotSpeed},
                {"angle", -90.f, 91.f, &angle},
                {"gain", 0.1f, 1.5f, &gain}};
    }

private:
    static constexpr int kRotDiv = 30;     // points per ring
    static constexpr int kRotHeight = 256; // generations kept
    static constexpr int kMapSize = 64;
    struct Point {
        float radius = 0, dRadius = 0, height = 0, dHeight = 0;
        float ax = 0, ay = 0;
        float r = 0, g = 0, b = 0;
    };
    std::vector<Point> _points;            // kRotHeight * kRotDiv
    float _map[kMapSize][3];
    double _rotation = 0.0;                // degrees
};

// Our take on AVS "Timescope" (e_timescope.cpp, BSD-3): a scrolling
// spectrogram. Each frame the previous frame is shifted left one pixel and a
// new column, colored by per-band spectrum intensity, is written at the right
// edge (the original instead swept a write cursor across a static image).
class TimescopeEffect : public Effect {
public:
    const char* name() const override { return "Timescope"; }
    bool usesPrev() const override { return true; }
    void render(Framebuffer& cur, const Framebuffer& prev,
                const VizFrame& a, const EffectContext& ctx) override;

    float hue   = 0.33f;  // column tint (orig: single color picker)
    float bands = 512.f;  // spectrum bands mapped down the column (truncated)
    float gain  = 1.0f;

    std::vector<Param> params() override {
        return {{"hue", 0.f, 1.f, &hue},
                {"bands", 16.f, 512.f, &bands},
                {"gain", 0.1f, 2.f, &gain}};
    }
};

// Our take on AVS Moving Particle (e_movingparticle.cpp, BSD-3): a single
// filled circle on a damped spring chasing a random target that jumps on each
// beat; size interpolates back after an on-beat pop.
class MovingParticleEffect : public Effect {
public:
    const char* name() const override { return "Moving Particle"; }
    void render(Framebuffer& cur, const Framebuffer& prev,
                const VizFrame& a, const EffectContext& ctx) override;

    float colR = 1.f, colG = 1.f, colB = 1.f;
    float distance = 16.f;    // 0..64, AVS units (/32 of the swing radius)
    float size = 8.f;         // 1..128 px
    float onBeatSizeChange = 0.f;
    float onBeatSize = 8.f;
    float blendMode = 3.f;    // 0=replace 1=additive(default) 2=5050 3=additive

    std::vector<Param> params() override {
        return {{"col_r", 0.f, 1.f, &colR}, {"col_g", 0.f, 1.f, &colG},
                {"col_b", 0.f, 1.f, &colB},
                {"distance", 1.f, 64.f, &distance},
                {"size", 1.f, 128.f, &size},
                {"on_beat_size_change", 0.f, 1.f, &onBeatSizeChange},
                {"on_beat_size", 1.f, 128.f, &onBeatSize},
                {"blend", 0.f, 3.f, &blendMode}};
    }

private:
    double _p[2] = {-0.6, 0.3};   // position, AVS's initial values
    double _v[2] = {-0.0154, -0.0107};
    double _c[2] = {0, 0};        // spring target
    float _curSize = 8.f;
    uint32_t _rng = 0x5bd1e995u;
    float frand();
};

} // namespace viz
