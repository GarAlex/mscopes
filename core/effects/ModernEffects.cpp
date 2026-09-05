//
// ModernEffects.cpp — see ModernEffects.h.
//
#include "ModernEffects.h"
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

    if (a.beat) _flash = beatFlash;
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
    _emitCarry += emitRate * dt + (a.beat ? beatBurst : 0.f);
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
        int cx = (int)p.x, cy = (int)p.y;
        for (int dy = -rad; dy <= rad; ++dy)
            for (int dx = -rad; dx <= rad; ++dx) {
                float fall = std::exp(-(float)(dx * dx + dy * dy) * inv2s2) * fade;
                if (fall < 0.02f) continue;
                cur.addClamped(cx + dx, cy + dy,
                               rgb[0] * fall, rgb[1] * fall, rgb[2] * fall);
            }
    }
}

} // namespace viz
