//
// ModernEffects.cpp — see ModernEffects.h.
//
#include "ModernEffects.h"
#include "LineMode.h"
#include <algorithm>
#include <cmath>

namespace viz {

// ---------------------------------------------------------------------------
//  TrailsEffect
// ---------------------------------------------------------------------------
void TrailsEffect::render(Framebuffer& cur, const Framebuffer& prev,
                          const VizFrame& a, const EffectContext& ctx)
{
    if (cur.px.size() != prev.px.size() || cur.px.empty()) return;

    if (onBeat(a)) _flash = beatFlash;
    _flash *= std::pow(0.05, ctx.dt);
    float p = std::clamp(persistence - (float)_flash, 0.f, 0.98f);
    if (p <= 0.f) return;

    float* c = cur.px.data();
    const float* q = prev.px.data();
    const size_t n = cur.px.size();
    if (mode > 0.5f) {
        for (size_t i = 0; i < n; i += 4) {          // phosphor: max with decay
            c[i]     = std::max(c[i],     q[i]     * p);
            c[i + 1] = std::max(c[i + 1], q[i + 1] * p);
            c[i + 2] = std::max(c[i + 2], q[i + 2] * p);
        }
    } else {
        const float k = 1.f - p;
        for (size_t i = 0; i < n; i += 4) {          // crossfade blur
            c[i]     = c[i]     * k + q[i]     * p;
            c[i + 1] = c[i + 1] * k + q[i + 1] * p;
            c[i + 2] = c[i + 2] * k + q[i + 2] * p;
        }
    }
}

// ---------------------------------------------------------------------------
//  ParticleSystemEffect
// ---------------------------------------------------------------------------
float ParticleSystemEffect::frand()
{
    _rng ^= _rng << 13; _rng ^= _rng >> 17; _rng ^= _rng << 5;
    return (_rng & 0xFFFFFF) / (float)0x1000000;
}

static void hue2rgb(float h, float* rgb)
{
    h -= std::floor(h);
    rgb[0] = std::clamp(std::fabs(h * 6.f - 3.f) - 1.f, 0.f, 1.f);
    rgb[1] = std::clamp(2.f - std::fabs(h * 6.f - 2.f), 0.f, 1.f);
    rgb[2] = std::clamp(2.f - std::fabs(h * 6.f - 4.f), 0.f, 1.f);
}

void ParticleSystemEffect::render(Framebuffer& cur, const Framebuffer& /*prev*/,
                                  const VizFrame& a, const EffectContext& ctx)
{
    const int W = cur.w, H = cur.h;
    if (W == 0 || H == 0) return;
    const float dt = (float)std::clamp(ctx.dt, 1.0 / 240.0, 1.0 / 15.0);
    const float unit = H * 0.5f;                 // half-height = 1 unit
    const size_t cap = (size_t)std::clamp((int)maxCount, 32, 4000);

    // --- emit ---
    _emitCarry += emitRate * dt + (onBeat(a) ? beatBurst : 0.f);
    int born = (int)_emitCarry;
    _emitCarry -= born;
    float v0 = speed * (1.f + speedBass * a.bass);
    for (int k = 0; k < born && _ps.size() < cap; ++k) {
        float ang = frand() * 6.2831853f;
        float mag = v0 * (0.4f + 0.6f * frand());
        P p;
        p.x = W * 0.5f; p.y = H * 0.5f;
        p.vx = std::cos(ang) * mag * unit;
        p.vy = std::sin(ang) * mag * unit;
        p.maxLife = p.life = lifetime * (0.5f + 0.5f * frand());
        p.hue = hue + (frand() - 0.5f) * hueSpread;
        _ps.push_back(p);
    }

    // --- integrate + cull ---
    const float dragK = std::exp(-drag * dt);
    const float g = gravity * unit * dt;
    for (auto& p : _ps) {
        p.vx *= dragK; p.vy *= dragK;
        p.vy += g;
        p.x += p.vx * dt; p.y += p.vy * dt;
        p.life -= dt;
    }
    _ps.erase(std::remove_if(_ps.begin(), _ps.end(), [&](const P& p) {
        return p.life <= 0 || p.x < -20 || p.x > W + 20 || p.y < -20 || p.y > H + 20;
    }), _ps.end());

    // --- draw: additive gaussian sprites, brightness fades with life ---
    const int rad = std::max(1, (int)size);
    const float inv2s2 = 1.f / (2.f * (size * 0.5f) * (size * 0.5f) + 1e-3f);
    float rgb[3];
    for (const auto& p : _ps) {
        float fade = p.life / p.maxLife;
        fade = fade * fade * (3.f - 2.f * fade);         // ease out
        hue2rgb(p.hue, rgb);
        // Sub-pixel: the gaussian is evaluated at each pixel centre's true
        // distance from the particle, so slow particles glide, not step.
        int cx = (int)std::floor(p.x), cy = (int)std::floor(p.y);
        for (int dy = -rad; dy <= rad + 1; ++dy)
            for (int dx = -rad; dx <= rad + 1; ++dx) {
                const float ex = cx + dx + 0.5f - p.x, ey = cy + dy + 0.5f - p.y;
                float fall = std::exp(-(ex * ex + ey * ey) * inv2s2) * fade;
                if (fall < 0.02f) continue;
                addLight(cur, cx + dx, cy + dy, rgb[0] * fall, rgb[1] * fall, rgb[2] * fall);
            }
    }
}

} // namespace viz

// ---------------------------------------------------------------------------
namespace viz {

void VectorscopeEffect::render(Framebuffer& cur, const Framebuffer& /*prev*/,
                               const VizFrame& a, const EffectContext& ctx)
{
    const int W = cur.w, H = cur.h;
    if (W == 0 || H == 0) return;

    // Every sample since the previous frame; a source without them gives
    // only the newest window, which overlaps the last one, so the beam
    // isn't joined across frames then.
    const float* L; const float* R; int n;
    if (a.hasFeatures && a.recentCount > 1) { L = a.recent[0]; R = a.recent[1]; n = a.recentCount; }
    else { L = a.waveform[0]; R = a.waveform[1]; n = kWaveformSamples; _haveLast = false; }

    const double dt = std::max(1e-3, ctx.dt);
    const bool phase = mode > 0.5f;
    const int d = std::clamp((int)delay, 1, 200);

    // The beam: stereo (left, right) or phase (the signal, and itself d
    // samples earlier), through a gentle low-pass like a real scope's
    // limited bandwidth, so hiss and cymbals don't scatter it. Then turned
    // 45° (mid up, side across).
    std::vector<float>& xs = _xs;
    std::vector<float>& ys = _ys;
    xs.resize((size_t)n); ys.resize((size_t)n);
    const float lpA = 1.f - (float)std::exp(-2.0 * M_PI * 1500.0 / 48000.0);
    float pk = 0.f;
    double sX = 0.0, sY = 0.0;
    for (int i = 0; i < n; ++i) {
        float u, v;
        if (phase) {
            u = 0.5f * (L[i] + R[i]);
            v = i >= d ? 0.5f * (L[i - d] + R[i - d]) : _hist[256 - d + i];
        } else {
            u = L[i]; v = R[i];
        }
        pk = std::max(pk, std::max(std::fabs(u), std::fabs(v)));
        _lpU += lpA * (u - _lpU);
        _lpV += lpA * (v - _lpV);
        xs[(size_t)i] = (_lpU - _lpV) * 0.70710678f;
        ys[(size_t)i] = (_lpU + _lpV) * 0.70710678f;
        sX += (double)xs[(size_t)i] * xs[(size_t)i];
        sY += (double)ys[(size_t)i] * ys[(size_t)i];
    }
    // Phase mode reaches back up to 200 samples into the previous frame.
    for (int j = 0; j < 256; ++j) {
        int i = n - 256 + j;
        _hist[j] = i >= 0 ? 0.5f * (L[i] + R[i]) : _hist[std::min(255, j + n)];
    }
    if (pk < 0.003f) { _haveLast = false; return; }       // silence: no beam

    // Follow the level: the RMS of each axis, smoothed over about half a
    // second, fills the scope to about half its radius, so music
    // (not its loudest transient) sets the size and peaks reach the edge.
    // Phase mode scales its axes separately, since low notes barely open
    // the loop sideways; stereo keeps one scale, so the width reads true.
    const float rX = (float)std::sqrt(sX / n), rY = (float)std::sqrt(sY / n);
    const float a0 = 1.f - (float)std::exp(-dt / 0.5);
    _peakX += ((phase ? rX : std::max(rX, rY)) - _peakX) * a0;
    _peakY += ((phase ? rY : std::max(rX, rY)) - _peakY) * a0;
    _peakX = std::max(_peakX, 0.003f); _peakY = std::max(_peakY, 0.003f);
    const float half = 0.5f * (float)std::min(W, H) * size;
    const float sx = (normalize > 0.5f ? 0.45f / _peakX : 1.f) * half;
    const float sy = (normalize > 0.5f ? 0.45f / _peakY : 1.f) * half;
    const float cx = W * 0.5f, cy = H * 0.5f;
    auto point = [&](int i, float& x, float& y) {
        x = cx + xs[(size_t)i] * sx;
        y = cy - ys[(size_t)i] * sy;
    };

    float rgb[3];
    hue2rgb(hue + hueSpeed * (float)ctx.time, rgb);
    for (float& c : rgb) c = 0.25f + 0.75f * c;             // phosphor: a little white in every hue
    const float hw = 0.5f * width * std::max(0.25f, drawQuality().widthScale);
    // The beam spends 1/rate on each segment: the longer the segment, the
    // thinner its light on each pixel.
    const float k = 3.f * gain * std::max(1.f, drawQuality().widthScale);

    float px, py, ppx = 0.f, ppy = 0.f;
    int i0 = 0;
    if (_haveLast) { px = _lx; py = _ly; }
    else { point(0, px, py); i0 = 1; }
    bool first = true;
    for (int i = i0; i < n; ++i) {
        float x, y;
        point(i, x, y);
        const float len = std::hypot(x - px, y - py);
        const float e = std::min(k / std::max(len, 0.7f), 1.5f);
        const bool capStart = first || sharpTurn(ppx, ppy, px, py, x, y);
        drawSegmentAA(cur, px, py, x, y, hw, rgb[0] * e, rgb[1] * e, rgb[2] * e,
                      /*additive*/1, 1.f, capStart, i == n - 1);
        ppx = px; ppy = py; px = x; py = y;
        first = false;
    }
    _lx = px; _ly = py; _haveLast = a.hasFeatures && a.recentCount > 1;
}

} // namespace viz
