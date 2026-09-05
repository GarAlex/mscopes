//
// GpuEffects.cpp — see GpuEffects.h.
//
#include "GpuEffects.h"
#include "GpuFx.h"
#include "EelToMsl.h"
#include <algorithm>
#include <cmath>

namespace viz {

void BloomEffect::render(Framebuffer& cur, const Framebuffer& /*prev*/,
                         const VizFrame& a, const EffectContext& /*ctx*/)
{
    float k = intensity * (1.f + bassBoost * a.bass);
    gpu::bloom(cur, threshold, radius, k);
}

void KaleidoscopeEffect::render(Framebuffer& cur, const Framebuffer& /*prev*/,
                                const VizFrame& a, const EffectContext& ctx)
{
    double dt = _lastTime < 0 ? 1.0 / 60.0 : std::max(0.0, ctx.time - _lastTime);
    _lastTime = ctx.time;
    if (a.beat) _kick += spinBeat;
    _kick *= std::pow(0.05, dt);               // fast decay
    _angle += (spin + _kick) * dt;
    gpu::kaleidoscope(cur, std::clamp((int)segments, 2, 16), (float)_angle, zoom);
}

void RgbSplitEffect::render(Framebuffer& cur, const Framebuffer& /*prev*/,
                            const VizFrame& a, const EffectContext& ctx)
{
    double dt = _lastTime < 0 ? 1.0 / 60.0 : std::max(0.0, ctx.time - _lastTime);
    _lastTime = ctx.time;
    if (a.beat) _pump = beatPump;
    _pump *= std::pow(0.02, dt);
    float amt = amount + (float)_pump;
    if (amt < 0.05f) return;
    gpu::rgbSplit(cur, amt, angle);
}

void ShockwaveEffect::render(Framebuffer& cur, const Framebuffer& /*prev*/,
                             const VizFrame& a, const EffectContext& ctx)
{
    double dt = _lastTime < 0 ? 1.0 / 60.0 : std::max(0.0, ctx.time - _lastTime);
    _lastTime = ctx.time;

    if (a.beat && a.bass >= bassGate) {
        for (auto& r : _radii)                   // claim a free (or oldest) slot
            if (r < 0) { r = 0.01f; goto spawned; }
        _radii[0] = 0.01f;
    }
spawned:
    bool any = false;
    for (auto& r : _radii) {
        if (r < 0) continue;
        r += speed * (float)dt;
        if (r > 2.6f) r = -1;                    // off both screen edges
        else any = true;
    }
    if (any) gpu::shockwave(cur, _radii, width, strength);
}

void GlitchEffect::render(Framebuffer& cur, const Framebuffer& /*prev*/,
                          const VizFrame& a, const EffectContext& ctx)
{
    double dt = _lastTime < 0 ? 1.0 / 60.0 : std::max(0.0, ctx.time - _lastTime);
    _lastTime = ctx.time;

    if (a.beat) _energy = 1.0;
    _energy *= std::exp(-decay * dt);
    float amt = std::clamp((float)_energy * intensity + constant, 0.f, 1.f);
    if (amt < 0.02f) return;

    _seed = (float)std::fmod(_seed + 7.31 + ctx.time * 13.7, 977.0);
    gpu::glitch(cur, amt, _seed, blockSize, tear);
}

void StreaksEffect::render(Framebuffer& cur, const Framebuffer& /*prev*/,
                           const VizFrame& /*a*/, const EffectContext& /*ctx*/)
{
    gpu::streaks(cur, threshold, length, intensity, tintR, tintG, tintB);
}

void LensEffect::render(Framebuffer& cur, const Framebuffer& /*prev*/,
                        const VizFrame& a, const EffectContext& /*ctx*/)
{
    float s = strength + bassBoost * a.bass * (strength >= 0 ? 1.f : -1.f);
    if (std::fabs(s) < 0.01f) return;
    gpu::lens(cur, s);
}

void RadialBlurEffect::render(Framebuffer& cur, const Framebuffer& /*prev*/,
                              const VizFrame& a, const EffectContext& ctx)
{
    double dt = _lastTime < 0 ? 1.0 / 60.0 : std::max(0.0, ctx.time - _lastTime);
    _lastTime = ctx.time;
    if (a.beat) _pump = beatPump;
    _pump *= std::pow(0.03, dt);
    float amt = std::clamp(amount + (float)_pump, 0.f, 1.f);
    if (amt < 0.02f) return;
    gpu::radialBlur(cur, amt, (int)quality);
}

void NeonEdgesEffect::render(Framebuffer& cur, const Framebuffer& /*prev*/,
                             const VizFrame& /*a*/, const EffectContext& /*ctx*/)
{
    gpu::edges(cur, glow, keepSource, tintR, tintG, tintB);
}

void DuotoneEffect::render(Framebuffer& cur, const Framebuffer& /*prev*/,
                           const VizFrame& /*a*/, const EffectContext& /*ctx*/)
{
    const float sh[3] = {shadowR, shadowG, shadowB};
    const float hi[3] = {highR, highG, highB};
    gpu::duotone(cur, sh, hi, mix);
}

void ShimmerEffect::render(Framebuffer& cur, const Framebuffer& /*prev*/,
                           const VizFrame& a, const EffectContext& ctx)
{
    double dt = _lastTime < 0 ? 1.0 / 60.0 : std::max(0.0, ctx.time - _lastTime);
    _lastTime = ctx.time;
    _t += speed * dt;
    float amt = amount + bassBoost * a.bass;
    if (amt < 0.05f) return;
    gpu::shimmer(cur, amt, scale, (float)_t);
}

void CRTEffect::render(Framebuffer& cur, const Framebuffer& /*prev*/,
                       const VizFrame& /*a*/, const EffectContext& /*ctx*/)
{
    gpu::crt(cur, curvature, scanlines, mask, corner);
}

void ToneMapEffect::render(Framebuffer& cur, const Framebuffer& /*prev*/,
                           const VizFrame& /*a*/, const EffectContext& /*ctx*/)
{
    gpu::toneMap(cur, exposure, gamma, saturation);
}

void VignetteEffect::render(Framebuffer& cur, const Framebuffer& /*prev*/,
                            const VizFrame& /*a*/, const EffectContext& /*ctx*/)
{
    gpu::vignette(cur, inner, outer, strength);
}

// ---------------------------------------------------------------------------
//  PixelShaderEffect
// ---------------------------------------------------------------------------
PixelShaderEffect::PixelShaderEffect()
{
    // default: audio-reactive plasma folded over the incoming frame
    _sInit = "";
    _sFrame = "speed=2+bass*3;";
    _sBeat = "";
    _sPoint =
        "pl=0.5+0.5*sin(10*d - t*speed + noise(x*3+t, y*3)*2);"
        "c=hsv(frac(pl*0.5 + t*0.05), 0.8, pl);"
        "red=red*0.35 + getr(c);"
        "green=green*0.35 + getg(c);"
        "blue=blue*0.35 + getb(c);";
}

void PixelShaderEffect::bindVars(eel::VM& vm)
{
    _x = vm.var("x"); _y = vm.var("y");
    _d = vm.var("d"); _r = vm.var("r");
    _red = vm.var("red"); _green = vm.var("green"); _blue = vm.var("blue");
    _w = vm.var("w"); _h = vm.var("h");
    _t = vm.var("t");
}

void PixelShaderEffect::render(Framebuffer& cur, const Framebuffer& /*prev*/,
                               const VizFrame& a, const EffectContext& ctx)
{
    if (cur.w == 0) return;

    ensureCompiled();
    *_t = ctx.time;
    *_w = cur.w; *_h = cur.h;
    _time = ctx.time;
    runFrameScripts(a);

    if (_transpiledFrom != _sPoint || _msl.empty()) {
        eel::MslResult res = eel::transpileToMsl(_pPoint, {
            {_x, "x"}, {_y, "y"}, {_d, "d"}, {_r, "r"},
            {_red, "red"}, {_green, "green"}, {_blue, "blue"},
            {_w, "w"}, {_h, "h"},
        });
        _msl = res.source;
        _uniformSlots = res.uniforms;
        _error = res.error;
        _transpiledFrom = _sPoint;
    }
    if (_msl.empty()) return;                  // transpile failed; _error set

    std::vector<float> u((size_t)eel::kMslUniformBase + _uniformSlots.size(), 0.f);
    u[0] = (float)(_frameSeed++ % 65536);      // rand() seed, varies per frame
    u[1] = ctx.coordSX > 0 ? ctx.coordSX : cur.w * 0.5f;
    u[2] = ctx.coordSY > 0 ? ctx.coordSY : cur.h * 0.5f;
    for (size_t i = 0; i < _uniformSlots.size(); ++i)
        u[eel::kMslUniformBase + i] = (float)*_uniformSlots[i];

    std::string err;
    if (!gpu::runCustomKernel(cur, _msl, u.data(), (int)u.size(), &err))
        _error = err;
    else
        _error.clear();
}

} // namespace viz
