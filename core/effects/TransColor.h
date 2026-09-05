//
// TransColor.h — clean-room ports of the classic AVS "Trans" color effects.
//
// Algorithms adapted from the BSD-3-licensed AVS sources (Copyright 2005
// Nullsoft, Inc.), re-expressed for our float RGBA 0..1 framebuffer.
//
#pragma once
#include "Effect.h"

namespace viz {

// Our take on AVS Brightness (e_brightness.cpp, BSD-3). Per-channel gain with
// the original's asymmetric response: negative slider fades toward black
// (factor 0..1), positive boosts hard (factor 1..17) — so max covers the old
// Fast Brightness 2x (and far beyond). Blend modes: replace / additive / 50:50.
class BrightnessEffect : public Effect {
public:
    const char* name() const override { return "Brightness"; }
    bool isGpu() const override;       // Metal when available, CPU loop otherwise
    void render(Framebuffer& cur, const Framebuffer& prev,
                const VizFrame& a, const EffectContext& ctx) override;

    float red      = 0.f;   // -4096..4096, AVS slider units
    float green    = 0.f;
    float blue     = 0.f;
    float separate = 0.f;   // <0.5: red slider drives all three channels
    float blend    = 0.f;   // 0 replace, 1 additive, 2 fifty-fifty (truncated)

    std::vector<Param> params() override {
        return {{"red", -4096.f, 4096.f, &red},
                {"green", -4096.f, 4096.f, &green},
                {"blue", -4096.f, 4096.f, &blue},
                {"separate", 0.f, 1.f, &separate},
                {"blend", 0.f, 2.f, &blend}};
    }
};

// Our take on AVS Invert (e_invert.cpp, BSD-3). RGB -> 1-RGB, alpha untouched.
class InvertEffect : public Effect {
public:
    const char* name() const override { return "Invert"; }
    bool isGpu() const override;       // Metal when available, CPU loop otherwise
    void render(Framebuffer& cur, const Framebuffer& prev,
                const VizFrame& a, const EffectContext& ctx) override;
};

// Our take on AVS Color Clip (e_colorclip.cpp, BSD-3). Pixels below / above /
// near the input color (all three channels compared) are replaced by the
// output color. "Near" uses Euclidean RGB distance, radius = 2 * distance.
class ColorClipEffect : public Effect {
public:
    const char* name() const override { return "Color Clip"; }
    bool isGpu() const override;       // Metal when available, CPU loop otherwise
    void render(Framebuffer& cur, const Framebuffer& prev,
                const VizFrame& a, const EffectContext& ctx) override;

    float mode     = 0.f;    // 0 below, 1 above, 2 near (truncated)
    float inR = 0.125f, inG = 0.125f, inB = 0.125f;   // compare color
    float outR = 0.f, outG = 0.f, outB = 0.f;         // replacement color
    float distance = 0.06f;  // "near" radius/2, 0..0.25 (AVS 0..64 of 255)

    std::vector<Param> params() override {
        return {{"mode", 0.f, 2.f, &mode},
                {"in_r", 0.f, 1.f, &inR}, {"in_g", 0.f, 1.f, &inG},
                {"in_b", 0.f, 1.f, &inB},
                {"out_r", 0.f, 1.f, &outR}, {"out_g", 0.f, 1.f, &outG},
                {"out_b", 0.f, 1.f, &outB},
                {"distance", 0.f, 0.25f, &distance}};
    }
};

// Our take on AVS Colorfade (e_colorfade.cpp, BSD-3). Each pixel's channels
// get one of three fader offsets assigned by which channel is brightest
// (the classic channel-rebalance that "rotates" hues). Faders glide 1 step
// (1/255) per frame toward their targets; on beat they can jump to alternate
// values or randomize. (We port the fixed behavior, not the 2.81d fader-swap
// bug.)
class ColorFadeEffect : public Effect {
public:
    const char* name() const override { return "Colorfade"; }
    bool isGpu() const override;       // Metal when available, CPU loop otherwise
    void render(Framebuffer& cur, const Framebuffer& prev,
                const VizFrame& a, const EffectContext& ctx) override;

    float faderMax  = 8.f;   // offset for the brightest channel, -32..32 (of 255)
    float fader2nd  = -8.f;  // offset for the second channel
    float fader3rd  = -8.f;  // offset for the dimmest / gray channel
    float onBeat       = 1.f; // >0.5: use beat targets when a beat hits
    float onBeatRandom = 1.f; // >0.5: randomize faders on beat instead
    float beatMax = 8.f, beat2nd = -8.f, beat3rd = -8.f;

    std::vector<Param> params() override {
        return {{"fader_max", -32.f, 32.f, &faderMax},
                {"fader_2nd", -32.f, 32.f, &fader2nd},
                {"fader_3rd", -32.f, 32.f, &fader3rd},
                {"on_beat", 0.f, 1.f, &onBeat},
                {"on_beat_random", 0.f, 1.f, &onBeatRandom},
                {"beat_max", -32.f, 32.f, &beatMax},
                {"beat_2nd", -32.f, 32.f, &beat2nd},
                {"beat_3rd", -32.f, 32.f, &beat3rd}};
    }

private:
    // gliding current fader values (AVS's cur_*)
    float _curMax = 8.f, _cur2nd = -8.f, _cur3rd = -8.f;
    uint32_t _rng = 0x2545f491u;
    uint32_t urand();               // cheap xorshift
};

// Our take on AVS Color Reduction (e_colorreduction.cpp, BSD-3). Posterize:
// each channel is floored to one of N levels (AVS masked off low bits, which
// floors — brights drop slightly, matching the original look).
class ColorReductionEffect : public Effect {
public:
    const char* name() const override { return "Color Reduction"; }
    bool isGpu() const override;       // Metal when available, CPU loop otherwise
    void render(Framebuffer& cur, const Framebuffer& prev,
                const VizFrame& a, const EffectContext& ctx) override;

    float levels = 8.f;      // levels per channel, truncated to int, 2..256

    std::vector<Param> params() override {
        return {{"levels", 2.f, 256.f, &levels}};
    }
};

// Our take on AVS Unique Tone (e_uniquetone.cpp, BSD-3). Depth = max(R,G,B)
// (optionally inverted) scales a single tone color. Blend modes:
// replace / additive / 50:50.
class UniqueToneEffect : public Effect {
public:
    const char* name() const override { return "Unique Tone"; }
    bool isGpu() const override;       // Metal when available, CPU loop otherwise
    void render(Framebuffer& cur, const Framebuffer& prev,
                const VizFrame& a, const EffectContext& ctx) override;

    float toneR = 0.75f, toneG = 0.5f, toneB = 1.f;
    float invert = 0.f;      // >0.5 inverts the depth
    float blend  = 0.f;      // 0 replace, 1 additive, 2 fifty-fifty (truncated)

    std::vector<Param> params() override {
        return {{"tone_r", 0.f, 1.f, &toneR},
                {"tone_g", 0.f, 1.f, &toneG},
                {"tone_b", 0.f, 1.f, &toneB},
                {"invert", 0.f, 1.f, &invert},
                {"blend", 0.f, 2.f, &blend}};
    }
};

// Our take on AVS Channel Shift (e_channelshift.cpp, BSD-3). Permutes the RGB
// channels; on beat it can hop to a random permutation (writes the mode param
// so the UI follows, just like AVS did).
class ChannelShiftEffect : public Effect {
public:
    const char* name() const override { return "Channel Shift"; }
    bool isGpu() const override;       // Metal when available, CPU loop otherwise
    void render(Framebuffer& cur, const Framebuffer& prev,
                const VizFrame& a, const EffectContext& ctx) override;

    float mode = 1.f;          // 0 RGB, 1 GBR, 2 BRG, 3 RBG, 4 BGR, 5 GRB
    float onBeatRandom = 1.f;  // >0.5: pick a random mode on beat

    std::vector<Param> params() override {
        return {{"mode", 0.f, 5.f, &mode},
                {"on_beat_random", 0.f, 1.f, &onBeatRandom}};
    }

private:
    uint32_t _rng = 0x9e3779b9u;
};

// Our take on AVS Multiplier (e_multiplier.cpp, BSD-3). Scales brightness by
// powers of two, plus the two degenerate "infinite" modes: infinite root
// (only pure white survives, everything else goes black) and infinite square
// (anything non-black snaps to white).
class MultiplierEffect : public Effect {
public:
    const char* name() const override { return "Multiplier"; }
    bool isGpu() const override;       // Metal when available, CPU loop otherwise
    void render(Framebuffer& cur, const Framebuffer& prev,
                const VizFrame& a, const EffectContext& ctx) override;

    // 0 inf-root, 1 x8, 2 x4, 3 x2, 4 x1/2, 5 x1/4, 6 x1/8, 7 inf-square
    float mode = 3.f;

    std::vector<Param> params() override {
        return {{"mode", 0.f, 7.f, &mode}};
    }
};

// Our take on AVS Fadeout (e_fadeout.cpp, BSD-3). Every frame each channel
// steps toward the target color by the fade speed (snapping when within one
// step) — the classic decay-to-color used under nearly every scope.
class FadeoutEffect : public Effect {
public:
    const char* name() const override { return "Fadeout"; }
    bool isGpu() const override;       // Metal when available, CPU loop otherwise
    void render(Framebuffer& cur, const Framebuffer& prev,
                const VizFrame& a, const EffectContext& ctx) override;

    float speed = 16.f / 255.f;  // per-frame step, 0..92/255 like AVS
    float targetR = 0.f, targetG = 0.f, targetB = 0.f;

    std::vector<Param> params() override {
        return {{"speed", 0.f, 92.f / 255.f, &speed},
                {"target_r", 0.f, 1.f, &targetR},
                {"target_g", 0.f, 1.f, &targetG},
                {"target_b", 0.f, 1.f, &targetB}};
    }
};

} // namespace viz
