//
// TransColor.cpp — clean-room ports of the classic AVS "Trans" color effects.
//
// Algorithms adapted from the BSD-3-licensed AVS sources (Copyright 2005
// Nullsoft, Inc.), re-expressed for our float RGBA 0..1 framebuffer.
//
#include "TransColor.h"
#include "GpuFx.h"
#include <cmath>
#include <algorithm>

namespace viz {

static inline float clamp01(float v) { return std::min(1.f, std::max(0.f, v)); }

// The three simple AVS blend modes, applied in place to one channel.
// mode: 0 replace, 1 additive, 2 fifty-fifty.
static inline float blendSimple(int mode, float src, float dst) {
    switch (mode) {
        case 1:  return std::min(1.f, src + dst);
        case 2:  return (src + dst) * 0.5f;
        default: return src;
    }
}

// ---------------------------------------------------------------------------
// Our take on AVS Brightness (e_brightness.cpp, BSD-3).
bool BrightnessEffect::isGpu() const { return gpu::available(); }

void BrightnessEffect::render(Framebuffer& cur, const Framebuffer&,
                              const VizFrame&, const EffectContext&)
{
    // AVS: factor = 1 + (slider < 0 ? 1 : 16) * slider/4096, i.e. -4096 -> 0,
    // 0 -> 1, +4096 -> 17. The old Fast Brightness "2x" is red=green=blue=256.
    auto factor = [](float v) { return 1.f + (v < 0.f ? 1.f : 16.f) * (v / 4096.f); };
    float fr = factor(red);
    float fg = (separate >= 0.5f) ? factor(green) : fr;
    float fb = (separate >= 0.5f) ? factor(blue)  : fr;
    int bm = (int)blend;

    if (gpu::available()) {
        const float q[4] = {fr, fg, fb, (float)bm};
        gpu::colorOp(cur, 0, q, 4);
        return;
    }

    float* p = cur.px.data();
    size_t n = (size_t)cur.w * cur.h;
    for (size_t i = 0; i < n; ++i, p += 4) {
        p[0] = blendSimple(bm, clamp01(p[0] * fr), p[0]);
        p[1] = blendSimple(bm, clamp01(p[1] * fg), p[1]);
        p[2] = blendSimple(bm, clamp01(p[2] * fb), p[2]);
    }
    quantize8(cur.px);            // 8-bit truncation, as AVS (Framebuffer.h)
}

// ---------------------------------------------------------------------------
// Our take on AVS Invert (e_invert.cpp, BSD-3): framebuffer ^= 0xffffff.
bool InvertEffect::isGpu() const { return gpu::available(); }

void InvertEffect::render(Framebuffer& cur, const Framebuffer&,
                          const VizFrame&, const EffectContext&)
{
    if (gpu::available()) { gpu::colorOp(cur, 1, nullptr, 0); return; }
    float* p = cur.px.data();
    size_t n = (size_t)cur.w * cur.h;
    for (size_t i = 0; i < n; ++i, p += 4) {
        p[0] = 1.f - p[0];
        p[1] = 1.f - p[1];
        p[2] = 1.f - p[2];
    }
    quantize8(cur.px);            // 8-bit truncation, as AVS (Framebuffer.h)
}

// ---------------------------------------------------------------------------
// Our take on AVS Color Clip (e_colorclip.cpp, BSD-3).
bool ColorClipEffect::isGpu() const { return gpu::available(); }

void ColorClipEffect::render(Framebuffer& cur, const Framebuffer&,
                             const VizFrame&, const EffectContext&)
{
    int m = (int)mode;
    // AVS "near": squared radius (2 * distance)^2 in channel units.
    float d = distance * 2.f;
    float d2 = d * d;

    if (gpu::available()) {
        const float q[8] = {(float)m, inR, inG, inB, outR, outG, outB, d2};
        gpu::colorOp(cur, 8, q, 8);
        return;
    }

    float* p = cur.px.data();
    size_t n = (size_t)cur.w * cur.h;
    for (size_t i = 0; i < n; ++i, p += 4) {
        bool hit;
        if (m == 0) {          // below
            hit = p[0] <= inR && p[1] <= inG && p[2] <= inB;
        } else if (m == 1) {   // above
            hit = p[0] >= inR && p[1] >= inG && p[2] >= inB;
        } else {               // near
            float dr = p[0] - inR, dg = p[1] - inG, db = p[2] - inB;
            hit = dr * dr + dg * dg + db * db <= d2;
        }
        if (hit) { p[0] = outR; p[1] = outG; p[2] = outB; }
    }
    quantize8(cur.px);            // 8-bit truncation, as AVS (Framebuffer.h)
}

// ---------------------------------------------------------------------------
// Our take on AVS Colorfade (e_colorfade.cpp, BSD-3).
uint32_t ColorFadeEffect::urand()
{
    _rng ^= _rng << 13; _rng ^= _rng >> 17; _rng ^= _rng << 5;
    return _rng;
}

bool ColorFadeEffect::isGpu() const { return gpu::available(); }

void ColorFadeEffect::render(Framebuffer& cur, const Framebuffer&,
                             const VizFrame& a, const EffectContext&)
{
    // Glide current faders one step (of 255) per frame toward the targets.
    auto step = [](float& c, float target) {
        if (c < target) c = std::min(target, c + 1.f);
        else if (c > target) c = std::max(target, c - 1.f);
    };
    step(_cur2nd, fader2nd);
    step(_curMax, faderMax);
    step(_cur3rd, fader3rd);

    if (onBeat < 0.5f) {
        _cur2nd = fader2nd; _curMax = faderMax; _cur3rd = fader3rd;
    } else if (a.beat && onBeatRandom >= 0.5f) {
        // AVS's exact random ranges, with the "not too subtle" fixup on max.
        _cur2nd = (float)((int)(urand() % 32) - 6);
        int m = (int)(urand() % 64) - 32;
        if (m < 0 && m > -16) m = -32;
        if (m >= 0 && m < 16) m = 32;
        _curMax = (float)m;
        _cur3rd = (float)((int)(urand() % 32) - 6);
    } else if (a.beat) {
        _cur2nd = beat2nd; _curMax = beatMax; _cur3rd = beat3rd;
    }

    // Fader assignment per brightness ordering (AVS's fader_switch table):
    // row = {offset for R, for G, for B}, chosen by the brightest channel.
    const float f2 = _cur2nd / 255.f, fm = _curMax / 255.f, f3 = _cur3rd / 255.f;
    const float sw[4][3] = {
        {f3, fm, f2},   // green brightest
        {fm, f2, f3},   // red brightest
        {f2, f3, fm},   // blue brightest
        {f3, f3, f3},   // all equal (grayscale)
    };

    if (gpu::available()) { gpu::colorOp(cur, 5, &sw[0][0], 12); return; }

    float* p = cur.px.data();
    size_t n = (size_t)cur.w * cur.h;
    for (size_t i = 0; i < n; ++i, p += 4) {
        float r = p[0], g = p[1], b = p[2];
        float gb = g - b, br = b - r;
        int s;
        if (gb > 0.f && gb > -br)      s = 0;  // g > b, g > r
        else if (br < 0.f && gb < -br) s = 1;  // r > b, r > g
        else if (gb < 0.f && br > 0.f) s = 2;  // b > g, b > r
        else                           s = 3;
        p[0] = clamp01(r + sw[s][0]);
        p[1] = clamp01(g + sw[s][1]);
        p[2] = clamp01(b + sw[s][2]);
    }
    quantize8(cur.px);            // 8-bit truncation, as AVS (Framebuffer.h)
}

// ---------------------------------------------------------------------------
// Our take on AVS Color Reduction (e_colorreduction.cpp, BSD-3).
bool ColorReductionEffect::isGpu() const { return gpu::available(); }

void ColorReductionEffect::render(Framebuffer& cur, const Framebuffer&,
                                  const VizFrame&, const EffectContext&)
{
    float lv = (float)std::max(2, (int)levels);
    if (gpu::available()) { gpu::colorOp(cur, 7, &lv, 1); return; }
    // AVS masked low bits off, i.e. floored each channel to its level's base
    // (so full white becomes the top level's base, slightly darker). floor()
    // against lv reproduces that: 1.0 -> (lv-1)/lv.
    auto posterize = [lv](float c) {
        float q = std::floor(clamp01(c) * lv);
        return std::min(q, lv - 1.f) / lv;
    };

    float* p = cur.px.data();
    size_t n = (size_t)cur.w * cur.h;
    for (size_t i = 0; i < n; ++i, p += 4) {
        p[0] = posterize(p[0]);
        p[1] = posterize(p[1]);
        p[2] = posterize(p[2]);
    }
    quantize8(cur.px);            // 8-bit truncation, as AVS (Framebuffer.h)
}

// ---------------------------------------------------------------------------
// Our take on AVS Unique Tone (e_uniquetone.cpp, BSD-3).
bool UniqueToneEffect::isGpu() const { return gpu::available(); }

void UniqueToneEffect::render(Framebuffer& cur, const Framebuffer&,
                              const VizFrame&, const EffectContext&)
{
    bool inv = invert >= 0.5f;
    int bm = (int)blend;

    if (gpu::available()) {
        const float q[5] = {toneR, toneG, toneB, inv ? 1.f : 0.f, (float)bm};
        gpu::colorOp(cur, 3, q, 5);
        return;
    }

    float* p = cur.px.data();
    size_t n = (size_t)cur.w * cur.h;
    for (size_t i = 0; i < n; ++i, p += 4) {
        float depth = std::max(p[0], std::max(p[1], p[2]));
        if (inv) depth = 1.f - depth;
        p[0] = blendSimple(bm, depth * toneR, p[0]);
        p[1] = blendSimple(bm, depth * toneG, p[1]);
        p[2] = blendSimple(bm, depth * toneB, p[2]);
    }
    quantize8(cur.px);            // 8-bit truncation, as AVS (Framebuffer.h)
}

// ---------------------------------------------------------------------------
// Our take on AVS Channel Shift (e_channelshift.cpp, BSD-3).
bool ChannelShiftEffect::isGpu() const { return gpu::available(); }

void ChannelShiftEffect::render(Framebuffer& cur, const Framebuffer&,
                                const VizFrame& a, const EffectContext&)
{
    if (a.beat && onBeatRandom >= 0.5f) {
        _rng ^= _rng << 13; _rng ^= _rng >> 17; _rng ^= _rng << 5;
        mode = (float)(_rng % 6);  // write through the param, like AVS did
    }
    int m = (int)mode;
    if (m <= 0 || m > 5) return;   // RGB = identity

    // Source-channel index for each destination channel (new RGB <- old
    // channels named by the mode).
    static const int perm[6][3] = {
        {0, 1, 2},  // RGB
        {1, 2, 0},  // GBR
        {2, 0, 1},  // BRG
        {0, 2, 1},  // RBG
        {2, 1, 0},  // BGR
        {1, 0, 2},  // GRB
    };
    const int* q = perm[m];

    if (gpu::available()) {
        const float qf[3] = {(float)q[0], (float)q[1], (float)q[2]};
        gpu::colorOp(cur, 4, qf, 3);
        return;
    }

    float* p = cur.px.data();
    size_t n = (size_t)cur.w * cur.h;
    for (size_t i = 0; i < n; ++i, p += 4) {
        float src[3] = {p[0], p[1], p[2]};
        p[0] = src[q[0]];
        p[1] = src[q[1]];
        p[2] = src[q[2]];
    }
    quantize8(cur.px);            // 8-bit truncation, as AVS (Framebuffer.h)
}

// ---------------------------------------------------------------------------
// Our take on AVS Multiplier (e_multiplier.cpp, BSD-3).
bool MultiplierEffect::isGpu() const { return gpu::available(); }

void MultiplierEffect::render(Framebuffer& cur, const Framebuffer&,
                              const VizFrame&, const EffectContext&)
{
    int m = (int)mode;
    if (gpu::available()) {
        static const float kFactors[6] = {8.f, 4.f, 2.f, 0.5f, 0.25f, 0.125f};
        const float q[2] = {(float)m, (m >= 1 && m <= 6) ? kFactors[m - 1] : 1.f};
        gpu::colorOp(cur, 6, q, 2);
        return;
    }
    float* p = cur.px.data();
    size_t n = (size_t)cur.w * cur.h;

    if (m == 0) {                    // infinite root: only pure white survives
        const float hi = 254.5f / 255.f;
        for (size_t i = 0; i < n; ++i, p += 4)
            if (p[0] < hi || p[1] < hi || p[2] < hi)
                p[0] = p[1] = p[2] = 0.f;
        return;
    }
    if (m >= 7) {                    // infinite square: any light becomes white
        const float lo = 0.5f / 255.f;
        for (size_t i = 0; i < n; ++i, p += 4)
            if (p[0] > lo || p[1] > lo || p[2] > lo)
                p[0] = p[1] = p[2] = 1.f;
        return;
    }
    // 1..6 -> x8, x4, x2, x1/2, x1/4, x1/8
    static const float factors[6] = {8.f, 4.f, 2.f, 0.5f, 0.25f, 0.125f};
    float f = factors[m - 1];
    for (size_t i = 0; i < n; ++i, p += 4) {
        p[0] = std::min(1.f, p[0] * f);
        p[1] = std::min(1.f, p[1] * f);
        p[2] = std::min(1.f, p[2] * f);
    }
    quantize8(cur.px);            // 8-bit truncation, as AVS (Framebuffer.h)
}

// ---------------------------------------------------------------------------
// Our take on AVS Fadeout (e_fadeout.cpp, BSD-3).
bool FadeoutEffect::isGpu() const { return gpu::available(); }

void FadeoutEffect::render(Framebuffer& cur, const Framebuffer&,
                           const VizFrame&, const EffectContext&)
{
    if (speed <= 0.f) return;
    const float tgt[3] = {targetR, targetG, targetB};

    if (gpu::available()) {
        const float q[4] = {speed, targetR, targetG, targetB};
        gpu::colorOp(cur, 2, q, 4);
        return;
    }

    float* p = cur.px.data();
    size_t n = (size_t)cur.w * cur.h;
    for (size_t i = 0; i < n; ++i, p += 4) {
        for (int c = 0; c < 3; ++c) {
            float v = p[c], t = tgt[c];
            if (v <= t - speed)      v += speed;
            else if (v >= t + speed) v -= speed;
            else                     v = t;
            p[c] = v;
        }
    }
    quantize8(cur.px);            // 8-bit truncation, as AVS (Framebuffer.h)
}

} // namespace viz
