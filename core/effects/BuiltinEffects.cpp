//
// BuiltinEffects.cpp
//
#include "BuiltinEffects.h"
#include "GpuFx.h"
#include <cmath>
#include <algorithm>

namespace viz {

// Simple HSV→RGB (h in [0,1)).
static void hsv(float h, float s, float v, float& r, float& g, float& b) {
    h = h - std::floor(h);
    float i = std::floor(h * 6.f);
    float f = h * 6.f - i;
    float p = v * (1 - s), q = v * (1 - s * f), t = v * (1 - s * (1 - f));
    switch (((int)i) % 6) {
        case 0: r=v; g=t; b=p; break;
        case 1: r=q; g=v; b=p; break;
        case 2: r=p; g=v; b=t; break;
        case 3: r=p; g=q; b=v; break;
        case 4: r=t; g=p; b=v; break;
        default:r=v; g=p; b=q; break;
    }
}

// ---------------------------------------------------------------------------
bool FeedbackWarp::isGpu() const { return gpu::available(); }

void FeedbackWarp::render(Framebuffer& cur, const Framebuffer& prev,
                          const VizFrame& a, const EffectContext& ctx)
{
    const int W = cur.w, H = cur.h;
    if (W == 0 || H == 0) return;
    const float cx = (W - 1) * 0.5f, cy = (H - 1) * 0.5f;

    float zoom = zoomBase + zoomBass * a.bass;
    float ang  = spin + spinTreble * a.treble;
    if (a.beat) zoom += beatKick;              // kick on beat
    // Inverse transform (we fill cur(x,y) by sampling prev at the source).
    float inv = 1.0f / zoom;
    float ca = std::cos(-ang) * inv, sa = std::sin(-ang) * inv;

    if (gpu::available() && prev.w == W && prev.h == H) {
        gpu::feedbackWarp(cur, prev, ca, sa, decay);
        return;
    }

    float px[4];
    for (int y = 0; y < H; ++y) {
        float dy = y - cy;
        for (int x = 0; x < W; ++x) {
            float dx = x - cx;
            float sx = cx + (dx * ca - dy * sa);
            float sy = cy + (dx * sa + dy * ca);
            prev.sample(sx, sy, px);
            float* d = cur.at(x, y);
            d[0] = px[0] * decay;
            d[1] = px[1] * decay;
            d[2] = px[2] * decay;
            d[3] = 1.f;
        }
    }
}

// ---------------------------------------------------------------------------
void ScopeEffect::render(Framebuffer& cur, const Framebuffer& /*prev*/,
                         const VizFrame& a, const EffectContext& ctx)
{
    const int W = cur.w, H = cur.h;
    if (W == 0 || H == 0) return;
    const float midY = H * 0.5f;
    float hueBase = (float)(ctx.frame % 600) / 600.f;

    // Waveform oscilloscope: one bright additive dot per column, hue cycling.
    float prevY = midY;
    for (int x = 0; x < W; ++x) {
        float t = (float)x / (W - 1);
        int wi = std::min((int)(t * kWaveformSamples), kWaveformSamples - 1);
        float s = a.waveform[0][wi];
        float y = midY + s * (H * amp);

        float r, g, b;
        hsv(hueBase + t * 0.25f, 0.85f, gain, r, g, b);

        // draw a short vertical span between prevY..y so the trace is continuous
        int y0 = (int)std::min(prevY, y), y1 = (int)std::max(prevY, y);
        y0 = std::clamp(y0, 0, H - 1); y1 = std::clamp(y1, 0, H - 1);
        for (int yy = y0; yy <= y1; ++yy) cur.addClamped(x, yy, r, g, b);
        prevY = y;
    }

    // Spectrum glow along the bottom: additive colored bars (short).
    const int bars = 96;
    for (int bar = 0; bar < bars; ++bar) {
        float f = (float)bar / bars;
        int lo = (int)(f * kSpectrumBins), hi = std::min((int)((f + 1.f/bars) * kSpectrumBins), kSpectrumBins);
        float e = 0; for (int i = lo; i < hi; ++i) e += a.spectrum[0][i];
        e /= std::max(1, hi - lo);
        int bh = (int)(e * H * 0.35f);
        float r, g, b; hsv(0.6f - f * 0.6f, 0.9f, gain * 0.7f, r, g, b);
        int x = (int)(f * W);
        for (int yy = 0; yy < bh; ++yy) cur.addClamped(x, yy, r, g, b);
    }
}

// ---------------------------------------------------------------------------
bool BlurEffect::isGpu() const { return gpu::available(); }

void BlurEffect::render(Framebuffer& cur, const Framebuffer& /*prev*/,
                        const VizFrame& /*a*/, const EffectContext& /*ctx*/)
{
    const int W = cur.w, H = cur.h, R = (int)radius;
    if (W == 0 || H == 0 || R <= 0) return;
    if (gpu::available()) { gpu::boxBlur(cur, R, std::clamp(mix, 0.f, 1.f)); return; }
    if ((int)_tmp.size() < W * H * 4) _tmp.resize((size_t)W * H * 4);

    const float norm = 1.0f / (2 * R + 1);

    // horizontal pass: cur -> _tmp
    for (int y = 0; y < H; ++y) {
        for (int x = 0; x < W; ++x) {
            float acc[3] = {0, 0, 0};
            for (int k = -R; k <= R; ++k) {
                int xx = std::clamp(x + k, 0, W - 1);
                const float* p = cur.at(xx, y);
                acc[0] += p[0]; acc[1] += p[1]; acc[2] += p[2];
            }
            float* t = &_tmp[((size_t)y * W + x) * 4];
            t[0] = acc[0] * norm; t[1] = acc[1] * norm; t[2] = acc[2] * norm;
        }
    }
    // vertical pass: _tmp -> cur (blended by mix)
    const float m = std::clamp(mix, 0.f, 1.f);
    for (int y = 0; y < H; ++y) {
        for (int x = 0; x < W; ++x) {
            float acc[3] = {0, 0, 0};
            for (int k = -R; k <= R; ++k) {
                int yy = std::clamp(y + k, 0, H - 1);
                const float* t = &_tmp[((size_t)yy * W + x) * 4];
                acc[0] += t[0]; acc[1] += t[1]; acc[2] += t[2];
            }
            float* d = cur.at(x, y);
            d[0] = d[0] + (acc[0] * norm - d[0]) * m;
            d[1] = d[1] + (acc[1] * norm - d[1]) * m;
            d[2] = d[2] + (acc[2] * norm - d[2]) * m;
        }
    }
    quantize8(cur.px);            // 8-bit truncation, as AVS (Framebuffer.h)
}

// ---------------------------------------------------------------------------
ColorMapEffect::ColorMapEffect()
{
    // Build a few gradient palettes (stops → 256-entry LUTs).
    struct Stop { float t, r, g, b; };
    const std::vector<std::vector<Stop>> defs = {
        // fire: black → red → orange → yellow → white
        {{0,0,0,0}, {0.35f,0.8f,0.05f,0}, {0.6f,1,0.5f,0}, {0.85f,1,0.9f,0.2f}, {1,1,1,1}},
        // ocean: black → deep blue → cyan → white
        {{0,0,0,0}, {0.4f,0,0.15f,0.55f}, {0.75f,0,0.8f,0.9f}, {1,1,1,1}},
        // toxic: black → green → yellow-green → white
        {{0,0,0,0}, {0.45f,0.1f,0.65f,0.1f}, {0.8f,0.7f,0.95f,0.1f}, {1,1,1,1}},
        // purple haze: black → violet → magenta → white
        {{0,0,0,0}, {0.4f,0.4f,0,0.7f}, {0.75f,0.95f,0.2f,0.8f}, {1,1,1,1}},
    };
    for (const auto& stops : defs) {
        Palette p;
        for (int i = 0; i < kLutSize; ++i) {
            float t = (float)i / (kLutSize - 1);
            // find surrounding stops
            size_t s = 0;
            while (s + 1 < stops.size() && stops[s + 1].t < t) ++s;
            const Stop& a = stops[s];
            const Stop& b = stops[std::min(s + 1, stops.size() - 1)];
            float span = std::max(1e-6f, b.t - a.t);
            float f = std::clamp((t - a.t) / span, 0.f, 1.f);
            p.lut[i][0] = a.r + (b.r - a.r) * f;
            p.lut[i][1] = a.g + (b.g - a.g) * f;
            p.lut[i][2] = a.b + (b.b - a.b) * f;
        }
        _palettes.push_back(p);
    }
}

bool ColorMapEffect::isGpu() const { return gpu::available(); }

void ColorMapEffect::render(Framebuffer& cur, const Framebuffer& /*prev*/,
                            const VizFrame& a, const EffectContext& /*ctx*/)
{
    if (_palettes.empty() || cur.w == 0) return;
    if (cycleOnBeat > 0.5f && a.beat)
        palette = (float)(((int)palette + 1) % (int)_palettes.size());
    const Palette& pal = _palettes[(size_t)palette % _palettes.size()];

    if (gpu::available()) { gpu::colorOp(cur, 9, nullptr, 0, &pal.lut[0][0], kLutSize * 3); return; }

    const int n = cur.w * cur.h;
    float* px = cur.px.data();
    for (int i = 0; i < n; ++i, px += 4) {
        // Rec.601 luma → LUT index
        float luma = 0.299f * px[0] + 0.587f * px[1] + 0.114f * px[2];
        int idx = (int)(std::clamp(luma, 0.f, 1.f) * (kLutSize - 1));
        px[0] = pal.lut[idx][0];
        px[1] = pal.lut[idx][1];
        px[2] = pal.lut[idx][2];
    }
    quantize8(cur.px);            // 8-bit truncation, as AVS (Framebuffer.h)
}

// ---------------------------------------------------------------------------
float StarfieldEffect::frand()
{
    _rng ^= _rng << 13; _rng ^= _rng >> 17; _rng ^= _rng << 5;
    return (_rng & 0xFFFFFF) / (float)0x1000000;
}

StarfieldEffect::StarfieldEffect()
{
    _stars.resize(320);
    for (auto& s : _stars) {
        s.x = frand() * 2.f - 1.f;
        s.y = frand() * 2.f - 1.f;
        s.z = 0.05f + frand() * 0.95f;
    }
}

void StarfieldEffect::render(Framebuffer& cur, const Framebuffer& /*prev*/,
                             const VizFrame& a, const EffectContext& ctx)
{
    const int W = cur.w, H = cur.h;
    if (W == 0 || H == 0) return;
    const float cx = W * 0.5f, cy = H * 0.5f;
    const float scale = 0.5f * std::min(W, H);
    const float dz = (speed + speedBass * a.bass) * (float)ctx.dt;

    for (auto& s : _stars) {
        s.z -= dz;
        if (s.z <= 0.05f) {                       // passed the camera → respawn far away
            s.x = frand() * 2.f - 1.f;
            s.y = frand() * 2.f - 1.f;
            s.z = 1.f;
        }
        float px = cx + s.x / s.z * scale;
        float py = cy + s.y / s.z * scale;
        float lum = brightness * (1.f - s.z) * (0.6f + 0.4f * a.mid);
        // 2x2 additive splat, slightly blue-white
        int ix = (int)px, iy = (int)py;
        for (int oy = 0; oy < 2; ++oy)
            for (int ox = 0; ox < 2; ++ox)
                cur.addClamped(ix + ox, iy + oy, lum, lum, std::min(1.f, lum * 1.15f));
    }
}

// ---------------------------------------------------------------------------
//  SpectrumBarsEffect — the original Music-plugin visual, parameterized
// ---------------------------------------------------------------------------
static void hsv2rgb(float h, float s, float v, float* rgb)
{
    h = h - std::floor(h);
    float r = std::fabs(h * 6.f - 3.f) - 1.f;
    float g = 2.f - std::fabs(h * 6.f - 2.f);
    float b = 2.f - std::fabs(h * 6.f - 4.f);
    rgb[0] = v * (1.f + s * (std::clamp(r, 0.f, 1.f) - 1.f));
    rgb[1] = v * (1.f + s * (std::clamp(g, 0.f, 1.f) - 1.f));
    rgb[2] = v * (1.f + s * (std::clamp(b, 0.f, 1.f) - 1.f));
}

void SpectrumBarsEffect::render(Framebuffer& cur, const Framebuffer& /*prev*/,
                                const VizFrame& a, const EffectContext& /*ctx*/)
{
    const int W = cur.w, H = cur.h;
    if (W == 0 || H == 0) return;

    if (background > 0.5f) cur.clear(0.03f, 0.03f, 0.05f);

    // spectrum bars from the bottom (plugin drew y-up; our rows are y-down)
    const int nBars = std::clamp((int)bars, 8, 128);
    const int perBar = std::max(1, kSpectrumBins / nBars);
    float rgb[3];
    for (int bar = 0; bar < nBars; ++bar) {
        float sum = 0.f;
        for (int k = 0; k < perBar; ++k)
            sum += a.spectrum[0][std::min(kSpectrumBins - 1, bar * perBar + k)];
        float avg = std::clamp(sum / perBar, 0.f, 1.f);
        hsv2rgb((float)bar / nBars + hueShift, saturation, 0.55f + 0.45f * avg, rgb);

        int x0 = bar * W / nBars + 1, x1 = (bar + 1) * W / nBars - 1;
        int hpx = (int)(avg * barHeight * H);
        for (int y = H - 1; y >= H - 1 - hpx && y >= 0; --y)
            for (int x = x0; x <= x1 && x < W; ++x) {
                float* p = cur.at(x, y);
                p[0] = rgb[0]; p[1] = rgb[1]; p[2] = rgb[2];
            }
    }

    // waveform trace across the vertical center
    if (waveAmp > 0.f) {
        int prevY = -1;
        for (int x = 0; x < W; ++x) {
            int i = x * (kWaveformSamples - 1) / std::max(1, W - 1);
            int y = std::clamp((int)(H * 0.5f + a.waveform[0][i] * waveAmp * H),
                               0, H - 1);
            int yA = prevY < 0 ? y : prevY, yB = y;
            if (yA > yB) std::swap(yA, yB);
            for (int yy = yA; yy <= yB; ++yy) {
                float* p = cur.at(x, yy);
                p[0] = p[1] = p[2] = 0.9f;
            }
            prevY = y;
        }
    }
}

} // namespace viz
