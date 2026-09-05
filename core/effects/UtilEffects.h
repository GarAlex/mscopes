//
// UtilEffects.h — small utility effects (clear/fade housekeeping).
//
#pragma once
#include "Effect.h"

namespace viz {

// Our take on AVS Clear Screen (e_clearscreen.cpp, BSD-3): fill the frame with
// a solid color each frame, optionally only every Nth frame, optionally 50/50
// blended instead of replacing.
class ClearScreenEffect : public Effect {
public:
    const char* name() const override { return "Clear Screen"; }
    void render(Framebuffer& cur, const Framebuffer& prev,
                const VizFrame& a, const EffectContext& ctx) override;

    float r = 0.f, g = 0.f, b = 0.f;
    float everyN = 1.f;     // clear every Nth frame (1 = every frame)
    float blend  = 0.f;     // 0 = replace, 1 = 50/50 blend
    // AVS's "only first frame": clear once when this effect instance starts
    // running, never again — the standard way presets seed a feedback chain
    // (Movement/Interleave/etc.) once and then let it accumulate forever.
    // Getting this wrong (always clearing) permanently erases feedback
    // before it can ever build up.
    float onlyFirst = 0.f;

    std::vector<Param> params() override {
        return {{"r", 0.f, 1.f, &r}, {"g", 0.f, 1.f, &g}, {"b", 0.f, 1.f, &b},
                {"every_n", 1.f, 60.f, &everyN},
                {"blend", 0.f, 1.f, &blend},
                {"only_first", 0.f, 1.f, &onlyFirst}};
    }

private:
    bool _hasCleared = false;
};

// The AVS global framebuffer pool: 8 named buffers shared by every effect in
// the process (Buffer Save, and later Multi/Video Delay). Sized lazily by
// whoever writes them first.
Framebuffer& globalBuffer(int index);          // index clamped to 0..7

// Our take on AVS Set Render Mode (e_setrendermode.cpp, BSD-3): sets the
// global line/dot blend mode, its adjustable alpha and the line width that
// every scope and dot renderer after it uses (see LineMode.h). The stack
// toggle doubles as the original's own enabled bit: disabled = leave the
// mode alone.
class SetRenderModeEffect : public Effect {
public:
    const char* name() const override { return "Set Render Mode"; }
    void render(Framebuffer& cur, const Framebuffer& prev,
                const VizFrame& a, const EffectContext& ctx) override;

    float blend     = 1.f;   // 0 replace 1 additive 2 max 3 50/50 4 dest-src
                             // 5 src-dest 6 multiply 7 adjustable 8 xor 9 minimum
    float alpha     = 1.f;   // mode 7 opacity
    float lineWidth = 1.f;   // pixels

    std::vector<Param> params() override {
        return {{"blend", 0.f, 9.f, &blend},
                {"alpha", 0.f, 1.f, &alpha},
                {"line_width", 1.f, 32.f, &lineWidth}};
    }
};

// Our take on AVS Misc / Buffer Save (e_buffersave.cpp, BSD-3): copy the frame
// into one of the 8 global buffers, or blend a buffer back onto the frame —
// the classic way presets pass a layer across the stack.
class BufferSaveEffect : public Effect {
public:
    const char* name() const override { return "Buffer Save"; }
    void render(Framebuffer& cur, const Framebuffer& prev,
                const VizFrame& a, const EffectContext& ctx) override;

    float action = 0.f;      // 0 save, 1 restore, 2 alt save/restore, 3 alt restore/save
    float buffer = 0.f;      // which global buffer, 0..7
    float blendMode = 0.f;   // restore blend: 0 replace 1 50/50 2 add 3 every-other-pixel
                             // 4 sub1 5 every-other-line 6 xor 7 max 8 min 9 sub2
                             // 10 multiply 11 adjustable
    float adjustable = 0.5f; // mix for blend mode 11

    std::vector<Param> params() override {
        return {{"action", 0.f, 3.f, &action},
                {"buffer", 0.f, 7.f, &buffer},
                {"blend_mode", 0.f, 11.f, &blendMode},
                {"adjustable", 0.f, 1.f, &adjustable}};
    }

private:
    bool _toggle = false;    // for the alternating modes; flips every frame
};

// Our take on AVS OnBeat Clear (e_onbeatclear.cpp, BSD-3): clear the frame on
// every Nth beat (classic hard "flash cut" on the kick).
class OnBeatClearEffect : public Effect {
public:
    const char* name() const override { return "OnBeat Clear"; }
    void render(Framebuffer& cur, const Framebuffer& prev,
                const VizFrame& a, const EffectContext& ctx) override;

    float r = 0.f, g = 0.f, b = 0.f;
    float everyNBeats = 1.f;   // clear on every Nth beat
    float blend       = 0.f;   // 0 = replace, 1 = 50/50

    std::vector<Param> params() override {
        return {{"r", 0.f, 1.f, &r}, {"g", 0.f, 1.f, &g}, {"b", 0.f, 1.f, &b},
                {"every_n_beats", 1.f, 16.f, &everyNBeats},
                {"blend", 0.f, 1.f, &blend}};
    }

private:
    int _beatCount = 0;
};

// Our take on AVS Misc / Custom BPM (e_custombpm.cpp, BSD-3): rewrites the
// beat flag seen by every effect BELOW it in the stack. The host passes its
// own per-frame copy of the VizFrame precisely so this effect can mutate it.
class CustomBPMEffect : public Effect {
public:
    const char* name() const override { return "Custom BPM"; }
    void render(Framebuffer& cur, const Framebuffer& prev,
                const VizFrame& a, const EffectContext& ctx) override;

    float mode      = 0.f;   // 0 fixed-BPM, 1 skip every Nth, 2 invert
    float bpm       = 120.f; // fixed mode: beats per minute
    float skip      = 1.f;   // skip mode: drop this many beats between kept ones
    float skipFirst = 0.f;   // suppress the first N beats after load

    std::vector<Param> params() override {
        return {{"mode", 0.f, 2.f, &mode},
                {"bpm", 10.f, 400.f, &bpm},
                {"skip", 1.f, 16.f, &skip},
                {"skip_first", 0.f, 64.f, &skipFirst}};
    }

private:
    double _lastFire = -1e9;
    int _skipCount = 0, _beatsSeen = 0;
};

} // namespace viz
