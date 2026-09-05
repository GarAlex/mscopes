//
// ScriptedTrans.cpp — see ScriptedTrans.h.
//
#include "ScriptedTrans.h"
#include "GpuFx.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace viz {

// ---------------------------------------------------------------------------
//  ScriptedEffectBase
// ---------------------------------------------------------------------------
void ScriptedEffectBase::setScripts(std::string init, std::string frame,
                                    std::string beat, std::string point)
{
    _sInit = std::move(init);
    _sFrame = std::move(frame);
    _sBeat = std::move(beat);
    _sPoint = std::move(point);
    _scriptsDirty = true;
}

void ScriptedEffectBase::ensureCompiled()
{
    if (!_scriptsDirty && _vm) return;
    _vm = std::make_unique<eel::VM>();
    _b = _vm->var("b");

    // Audio hooks, same contract as Superscope's.
    auto sample = [](const float* buf, int len, double band, double width) {
        int lo = (int)std::floor(std::clamp(band - width * 0.5, 0.0, 1.0) * (len - 1));
        int hi = (int)std::ceil(std::clamp(band + width * 0.5, 0.0, 1.0) * (len - 1));
        if (hi < lo) std::swap(lo, hi);
        double s = 0; int cnt = 0;
        for (int k = lo; k <= hi; ++k) { s += buf[k]; ++cnt; }
        return cnt ? s / cnt : 0.0;
    };
    _vm->getosc = [this, sample](double band, double width, double ch) {
        if (!_audioFrame) return 0.0;
        int c = ((int)ch == 2) ? 1 : 0;
        double v = sample(_audioFrame->waveform[c], kWaveformSamples, band, width);
        if ((int)ch == 0 && _audioFrame->numWaveformChannels > 1)
            v = (v + sample(_audioFrame->waveform[1], kWaveformSamples, band, width)) * 0.5;
        return v;
    };
    _vm->getspec = [this, sample](double band, double width, double ch) {
        if (!_audioFrame) return 0.0;
        int c = ((int)ch == 2) ? 1 : 0;
        double v = sample(_audioFrame->spectrum[c], kSpectrumBins, band, width);
        if ((int)ch == 0 && _audioFrame->numSpectrumChannels > 1)
            v = (v + sample(_audioFrame->spectrum[1], kSpectrumBins, band, width)) * 0.5;
        return v;
    };
    _vm->gettime = [this](double start) { return _time - start; };

    bindVars(*_vm);
    _pInit  = eel::compile(*_vm, _sInit);
    _pFrame = eel::compile(*_vm, _sFrame);
    _pBeat  = eel::compile(*_vm, _sBeat);
    _pPoint = eel::compile(*_vm, _sPoint);
    _scriptsDirty = false;
    _inited = false;
}

void ScriptedEffectBase::runFrameScripts(const VizFrame& a)
{
    _audioFrame = &a;
    *_b = a.beat ? 1.0 : 0.0;
    *_vm->var("bass") = a.bass;
    *_vm->var("mid") = a.mid;
    *_vm->var("treb") = a.treble;
    *_vm->var("bpm") = a.bpm;
    *_vm->var("beatphase") = a.beatPhase;
    if (!_inited) { _pInit.run(); _inited = true; }
    if (a.beat) _pBeat.run();
    _pFrame.run();
}

// ---------------------------------------------------------------------------
//  ColorModifierEffect
// ---------------------------------------------------------------------------
ColorModifierEffect::ColorModifierEffect()
{
    // default: on-beat brightness pump (a classic bundled Color Modifier)
    _sInit = "c=200/255; f=0;";
    _sBeat = "c=1.3; f=1;";
    _sFrame = "f=f*0.95; c=max(200/255, c*f + (1-f)*200/255);";
    _sPoint = "red=red*c; green=green*c; blue=blue*c;";
}

void ColorModifierEffect::bindVars(eel::VM& vm)
{
    _red = vm.var("red");
    _green = vm.var("green");
    _blue = vm.var("blue");
    vm.var("beat");                // AVS name for the beat flag in this effect
    _lutValid = false;
}

void ColorModifierEffect::render(Framebuffer& cur, const Framebuffer& /*prev*/,
                                 const VizFrame& a, const EffectContext& /*ctx*/)
{
    ensureCompiled();
    *_vm->var("beat") = a.beat ? 1.0 : 0.0;
    runFrameScripts(a);

    if (!_lutValid || recompute > 0.5f) {
        for (int i = 0; i < 256; ++i) {
            double v = i / 255.0;
            *_red = v; *_green = v; *_blue = v;
            _pPoint.run();
            _lut[0][i] = (float)std::clamp(*_red, 0.0, 1.0);
            _lut[1][i] = (float)std::clamp(*_green, 0.0, 1.0);
            _lut[2][i] = (float)std::clamp(*_blue, 0.0, 1.0);
        }
        _lutValid = true;
    }

    float* p = cur.px.data();
    const size_t n = cur.px.size();
    for (size_t i = 0; i < n; i += 4) {
        p[i + 0] = _lut[0][(int)(std::clamp(p[i + 0], 0.f, 1.f) * 255.f + 0.5f)];
        p[i + 1] = _lut[1][(int)(std::clamp(p[i + 1], 0.f, 1.f) * 255.f + 0.5f)];
        p[i + 2] = _lut[2][(int)(std::clamp(p[i + 2], 0.f, 1.f) * 255.f + 0.5f)];
    }
}

// ---------------------------------------------------------------------------
//  DynamicShiftEffect
// ---------------------------------------------------------------------------
DynamicShiftEffect::DynamicShiftEffect()
{
    // default: jump on beat, ease back to center
    _sInit = "x=0; y=0;";
    _sBeat = "x=rand(81)-40; y=rand(81)-40;";
    _sFrame = "x=x*0.9; y=y*0.9;";
}

void DynamicShiftEffect::bindVars(eel::VM& vm)
{
    _x = vm.var("x"); _y = vm.var("y");
    _w = vm.var("w"); _h = vm.var("h");
    _alpha = vm.var("alpha");
    *_alpha = 0.5;                       // AVS default
}

bool DynamicShiftEffect::isGpu() const { return gpu::available(); }

void DynamicShiftEffect::render(Framebuffer& cur, const Framebuffer& /*prev*/,
                                const VizFrame& a, const EffectContext& /*ctx*/)
{
    const int W = cur.w, H = cur.h;
    if (W == 0 || H == 0) return;

    ensureCompiled();
    *_w = W; *_h = H;
    runFrameScripts(a);

    const double sx = *_x, sy = *_y;
    if (sx == 0.0 && sy == 0.0 && blend <= 0.5f) return;

    const bool doBlend = blend > 0.5f;
    const float al = (float)std::clamp(*_alpha, 0.0, 1.0);
    const bool subpx = bilinear > 0.5f;

    if (gpu::available()) { gpu::shift(cur, (float)sx, (float)sy, doBlend, al, subpx); return; }

    _src.resize(W, H);
    _src.px = cur.px;
    float sample[4];

    for (int y = 0; y < H; ++y) {
        for (int x = 0; x < W; ++x) {
            double fx = x - sx, fy = y - sy;
            float* dpx = cur.at(x, y);
            float r, g, b;
            if (fx < 0 || fy < 0 || fx > W - 1 || fy > H - 1) {
                r = g = b = 0.f;                        // uncovered area: black
            } else if (subpx) {
                _src.sample((float)fx, (float)fy, sample);
                r = sample[0]; g = sample[1]; b = sample[2];
            } else {
                const float* s = _src.at((int)fx, (int)fy);
                r = s[0]; g = s[1]; b = s[2];
            }
            if (doBlend) {
                dpx[0] += (r - dpx[0]) * al;
                dpx[1] += (g - dpx[1]) * al;
                dpx[2] += (b - dpx[2]) * al;
            } else {
                dpx[0] = r; dpx[1] = g; dpx[2] = b;
            }
        }
    }
    quantize8(cur.px);            // 8-bit truncation, as AVS (Framebuffer.h)
}

// ---------------------------------------------------------------------------
//  DynamicDistanceModifierEffect
// ---------------------------------------------------------------------------
DynamicDistanceModifierEffect::DynamicDistanceModifierEffect()
{
    // default: zoom pulse on beat
    _sInit = "dd=1;";
    _sBeat = "dd=1.25;";
    _sFrame = "dd=1+(dd-1)*0.9;";
    _sPoint = "d=d*dd;";
}

void DynamicDistanceModifierEffect::bindVars(eel::VM& vm)
{
    _d = vm.var("d");
}

bool DynamicDistanceModifierEffect::isGpu() const { return gpu::available(); }

void DynamicDistanceModifierEffect::render(Framebuffer& cur, const Framebuffer& /*prev*/,
                                           const VizFrame& a, const EffectContext& /*ctx*/)
{
    const int W = cur.w, H = cur.h;
    if (W == 0 || H == 0) return;

    prepareFrame(a);

    // Distance table, AVS-style: one entry per pixel of radius out to the
    // half-diagonal (+ margin); the point script maps normalized d in [0,1).
    const int maxD = std::max(33, (int)(std::sqrt((double)(W * W + H * H)) * 0.5 + 32.9));
    _table.resize((size_t)maxD);
    for (int i = 0; i < maxD; ++i) {
        *_d = (double)i / (maxD - 1);
        _pPoint.run();
        double out = *_d * (maxD - 1);              // remapped radius in pixels
        _table[i] = (float)(i > 0 ? out / i : out); // per-radius scale factor
    }

    const bool doBlend = blend > 0.5f;
    const bool subpx = bilinear > 0.5f;

    if (gpu::available()) { gpu::distanceModifier(cur, _table.data(), maxD, doBlend, subpx); return; }

    _src.resize(W, H);
    _src.px = cur.px;

    const double cx = (W - 1) * 0.5, cy = (H - 1) * 0.5;
    float sample[4];

    for (int y = 0; y < H; ++y) {
        for (int x = 0; x < W; ++x) {
            double dx = x - cx, dy = y - cy;
            double dist = std::sqrt(dx * dx + dy * dy);
            int di = std::min((int)dist, maxD - 1);
            double f = _table[di];
            double fx = std::clamp(cx + dx * f, 0.0, (double)(W - 1));
            double fy = std::clamp(cy + dy * f, 0.0, (double)(H - 1));

            float r, g, b;
            if (subpx) {
                _src.sample((float)fx, (float)fy, sample);
                r = sample[0]; g = sample[1]; b = sample[2];
            } else {
                const float* s = _src.at((int)fx, (int)fy);
                r = s[0]; g = s[1]; b = s[2];
            }
            float* dpx = cur.at(x, y);
            if (doBlend) {
                dpx[0] = (dpx[0] + r) * 0.5f;
                dpx[1] = (dpx[1] + g) * 0.5f;
                dpx[2] = (dpx[2] + b) * 0.5f;
            } else {
                dpx[0] = r; dpx[1] = g; dpx[2] = b;
            }
        }
    }
    quantize8(cur.px);            // 8-bit truncation, as AVS (Framebuffer.h)
}

// ---------------------------------------------------------------------------
//  TriangleEffect
// ---------------------------------------------------------------------------
TriangleEffect::TriangleEffect()
{
    // default: a bass-pumped fan of translucent-feeling triangles
    _sInit = "n=14;";
    _sFrame = "t=t+0.014; base=0.35+getspec(0.05,0.1,0)*0.7;";
    _sBeat = "t=t+0.2;";
    _sPoint =
        "a1=i/n*$PI*2+t; a2=(i+0.6)/n*$PI*2+t;"
        "x1=0; y1=0;"
        "x2=cos(a1)*base; y2=sin(a1)*base;"
        "x3=cos(a2)*base; y3=sin(a2)*base;"
        "red1=0.9; green1=0.9; blue1=1;"
        "red2=sin(i/n*$PI)*0.9; green2=0.2; blue2=1-i/n;"
        "red3=0.1; green3=0.5+sin(t)*0.3; blue3=0.9;";
}

void TriangleEffect::bindVars(eel::VM& vm)
{
    _n = vm.var("n"); _i = vm.var("i"); _skip = vm.var("skip");
    _x1 = vm.var("x1"); _y1 = vm.var("y1"); _z1 = vm.var("z1");
    _x2 = vm.var("x2"); _y2 = vm.var("y2");
    _x3 = vm.var("x3"); _y3 = vm.var("y3");
    _r1 = vm.var("red1"); _g1 = vm.var("green1"); _b1 = vm.var("blue1");
    _r2 = vm.var("red2"); _g2 = vm.var("green2"); _b2 = vm.var("blue2");
    _r3 = vm.var("red3"); _g3 = vm.var("green3"); _b3 = vm.var("blue3");
    _zbuf = vm.var("zbuf"); _zbclear = vm.var("zbclear");
    _w = vm.var("w"); _h = vm.var("h");
    *_r1 = *_g1 = *_b1 = 1.0;
    *_r2 = *_g2 = *_b2 = 1.0;
    *_r3 = *_g3 = *_b3 = 1.0;
}

void TriangleEffect::render(Framebuffer& cur, const Framebuffer& /*prev*/,
                            const VizFrame& a, const EffectContext& ctx)
{
    const int W = cur.w, H = cur.h;
    if (W == 0 || H == 0) return;

    ensureCompiled();
    _time = ctx.time;
    *_w = W; *_h = H;
    runFrameScripts(a);

    const int count = std::clamp((int)*_n, 0, 4096);
    const bool useZ = *_zbuf != 0.0;
    if (useZ) {
        if (_depth.size() != (size_t)W * H) _depth.assign((size_t)W * H, -1e30f);
        if (*_zbclear != 0.0) std::fill(_depth.begin(), _depth.end(), -1e30f);
    }

    for (int t = 0; t < count; ++t) {
        *_i = count > 1 ? (double)t / (count - 1) : 0.0;
        *_skip = 0;
        _pPoint.run();
        if (*_skip != 0.0) continue;

        // -1..1 → pixels (aspect-aware; 0 scale = classic stretch)
        const float SX = ctx.coordSX > 0 ? ctx.coordSX : W * 0.5f;
        const float SY = ctx.coordSY > 0 ? ctx.coordSY : H * 0.5f;
        float px[3] = {(float)(W * 0.5 + *_x1 * SX), (float)(W * 0.5 + *_x2 * SX),
                       (float)(W * 0.5 + *_x3 * SX)};
        float py[3] = {(float)(H * 0.5 + *_y1 * SY), (float)(H * 0.5 + *_y2 * SY),
                       (float)(H * 0.5 + *_y3 * SY)};
        float cr[3] = {(float)*_r1, (float)*_r2, (float)*_r3};
        float cg[3] = {(float)*_g1, (float)*_g2, (float)*_g3};
        float cb[3] = {(float)*_b1, (float)*_b2, (float)*_b3};
        float z = (float)*_z1;

        int minX = std::max(0, (int)std::floor(std::min({px[0], px[1], px[2]})));
        int maxX = std::min(W - 1, (int)std::ceil(std::max({px[0], px[1], px[2]})));
        int minY = std::max(0, (int)std::floor(std::min({py[0], py[1], py[2]})));
        int maxY = std::min(H - 1, (int)std::ceil(std::max({py[0], py[1], py[2]})));
        if (minX > maxX || minY > maxY) continue;

        float d = (py[1] - py[2]) * (px[0] - px[2]) + (px[2] - px[1]) * (py[0] - py[2]);
        if (std::fabs(d) < 1e-9f) continue;
        float invD = 1.0f / d;

        for (int y = minY; y <= maxY; ++y) {
            for (int x = minX; x <= maxX; ++x) {
                float w0 = ((py[1] - py[2]) * (x - px[2]) + (px[2] - px[1]) * (y - py[2])) * invD;
                float w1 = ((py[2] - py[0]) * (x - px[2]) + (px[0] - px[2]) * (y - py[2])) * invD;
                float w2 = 1.0f - w0 - w1;
                if (w0 < 0 || w1 < 0 || w2 < 0) continue;
                if (useZ) {
                    float& zp = _depth[(size_t)y * W + x];
                    if (z <= zp) continue;
                    zp = z;
                }
                float* dp = cur.at(x, y);
                dp[0] = std::clamp(w0 * cr[0] + w1 * cr[1] + w2 * cr[2], 0.f, 1.f);
                dp[1] = std::clamp(w0 * cg[0] + w1 * cg[1] + w2 * cg[2], 0.f, 1.f);
                dp[2] = std::clamp(w0 * cb[0] + w1 * cb[1] + w2 * cb[2], 0.f, 1.f);
            }
        }
    }
}

// ---------------------------------------------------------------------------
//  TexerIIEffect
// ---------------------------------------------------------------------------
static std::string g_resourceDir;
void setPresetResourceDir(const std::string& dir) { g_resourceDir = dir; }
const std::string& presetResourceDir() { return g_resourceDir; }

// Minimal BMP reader: uncompressed 24/32-bit, bottom-up or top-down.
// Returns rgb float triplets; empty on any parse problem.
static std::vector<float> loadBmp(const std::string& path, int& w, int& h)
{
    std::vector<float> out;
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) return out;
    std::vector<uint8_t> d;
    uint8_t buf[65536]; size_t n;
    while ((n = fread(buf, 1, sizeof buf, f)) > 0) d.insert(d.end(), buf, buf + n);
    fclose(f);

    auto u16 = [&](size_t o) { return d[o] | (d[o + 1] << 8); };
    auto u32 = [&](size_t o) { return (uint32_t)(d[o] | (d[o+1]<<8) | (d[o+2]<<16) | ((uint32_t)d[o+3]<<24)); };
    if (d.size() < 54 || d[0] != 'B' || d[1] != 'M') return out;
    uint32_t dataOff = u32(10);
    int32_t bw = (int32_t)u32(18), bh = (int32_t)u32(22);
    int bpp = u16(28);
    if (u32(30) != 0 || (bpp != 24 && bpp != 32)) return out;   // compressed / palette
    bool topDown = bh < 0;
    int H = topDown ? -bh : bh, W = bw;
    if (W <= 0 || H <= 0 || W > 4096 || H > 4096) return out;
    size_t stride = ((size_t)W * (bpp / 8) + 3) & ~(size_t)3;
    if (dataOff + stride * H > d.size()) return out;

    out.resize((size_t)W * H * 3);
    for (int y = 0; y < H; ++y) {
        int sy = topDown ? y : (H - 1 - y);
        const uint8_t* row = &d[dataOff + stride * sy];
        for (int x = 0; x < W; ++x) {
            const uint8_t* px = row + (size_t)x * (bpp / 8);
            float* o = &out[((size_t)y * W + x) * 3];
            o[0] = px[2] / 255.f;      // BMP stores BGR
            o[1] = px[1] / 255.f;
            o[2] = px[0] / 255.f;
        }
    }
    w = W; h = H;
    return out;
}

// 21x21 soft radial glow (the built-in fallback texture).
static void makeGlow(std::vector<float>& tex, int& w, int& h)
{
    const int S = 21;
    w = h = S;
    tex.resize((size_t)S * S * 3);
    const float c = (S - 1) * 0.5f;
    for (int y = 0; y < S; ++y)
        for (int x = 0; x < S; ++x) {
            float dx = (x - c) / c, dy = (y - c) / c;
            float v = std::max(0.f, std::exp(-(dx * dx + dy * dy) * 3.2f) - 0.04f);
            float* o = &tex[((size_t)y * S + x) * 3];
            o[0] = o[1] = o[2] = v;
        }
}

void TexerIIEffect::loadTexture()
{
    _tex.clear();
    if (!_imageName.empty() && !g_resourceDir.empty())
        _tex = loadBmp(g_resourceDir + "/" + _imageName, _texW, _texH);
    if (_tex.empty()) makeGlow(_tex, _texW, _texH);
    _texLoaded = true;
}

TexerIIEffect::TexerIIEffect()
{
    // default: a breathing ring of glow sprites
    _sInit = "n=24;";
    _sFrame = "t=t-0.01; sz=1+getspec(0.06,0.1,0)*2.5;";
    _sBeat = "t=t-0.12;";
    _sPoint =
        "a=i*$PI*2+t;"
        "rad=0.55+0.2*sin(t*2+i*$PI*6);"
        "x=cos(a)*rad; y=sin(a)*rad;"
        "sizex=sz; sizey=sz;"
        "red=0.5+0.5*sin(a); green=0.4; blue=0.5+0.5*cos(a);";
}

void TexerIIEffect::bindVars(eel::VM& vm)
{
    _n = vm.var("n"); _i = vm.var("i");
    _x = vm.var("x"); _y = vm.var("y");
    _w = vm.var("w"); _h = vm.var("h");
    _iw = vm.var("iw"); _ih = vm.var("ih");
    _sizex = vm.var("sizex"); _sizey = vm.var("sizey");
    _red = vm.var("red"); _green = vm.var("green"); _blue = vm.var("blue");
    _skip = vm.var("skip"); _v = vm.var("v");
    *_sizex = *_sizey = 1.0;
    *_red = *_green = *_blue = 1.0;
}

void TexerIIEffect::render(Framebuffer& cur, const Framebuffer& /*prev*/,
                           const VizFrame& a, const EffectContext& ctx)
{
    const int W = cur.w, H = cur.h;
    if (W == 0 || H == 0) return;

    ensureCompiled();
    if (!_texLoaded) loadTexture();
    _time = ctx.time;
    *_w = W; *_h = H;
    *_iw = _texW; *_ih = _texH;
    runFrameScripts(a);

    const float* tex = _tex.data();
    const int TW = _texW, TH = _texH;
    const int count = std::clamp((int)*_n, 0, 4096);
    const bool doWrap = wrap > 0.5f;
    const bool doColor = colorize > 0.5f;
    const int bm = (int)blendMode;

    for (int s = 0; s < count; ++s) {
        *_i = count > 1 ? (double)s / (count - 1) : 0.0;
        {   // v: center-channel waveform sample at this particle's fraction
            int wi = (int)(*_i * (kWaveformSamples - 1));
            *_v = a.waveform[0][wi];
        }
        *_skip = 0;
        _pPoint.run();
        if (*_skip != 0.0) continue;

        const float SX = ctx.coordSX > 0 ? ctx.coordSX : W * 0.5f;
        const float SY = ctx.coordSY > 0 ? ctx.coordSY : H * 0.5f;
        float cx = (float)(W * 0.5 + *_x * SX);
        float cy = (float)(H * 0.5 + *_y * SY);
        float halfW = (float)(TW * std::fabs(*_sizex) * 0.5);
        float halfH = (float)(TH * std::fabs(*_sizey) * 0.5);
        if (halfW < 0.25f || halfH < 0.25f) continue;
        float cr = doColor ? (float)std::clamp(*_red, 0.0, 1.0) : 1.f;
        float cg = doColor ? (float)std::clamp(*_green, 0.0, 1.0) : 1.f;
        float cb = doColor ? (float)std::clamp(*_blue, 0.0, 1.0) : 1.f;

        int x0 = (int)std::floor(cx - halfW), x1 = (int)std::ceil(cx + halfW);
        int y0 = (int)std::floor(cy - halfH), y1 = (int)std::ceil(cy + halfH);

        // Bound the raster loop by the screen. Scripts routinely grow a
        // sprite every frame without limit; iterating its full extent (and
        // skipping per pixel) made one community preset cost minutes per
        // frame. Unwrapped: clip — identical output. Wrapped: one tile's
        // worth of rows/columns (beyond that the sprite only aliases itself).
        int ys = y0, ye = y1, xs = x0, xe = x1;
        if (!doWrap) {
            ys = std::max(y0, 0); ye = std::min(y1, H - 1);
            xs = std::max(x0, 0); xe = std::min(x1, W - 1);
        } else {
            ye = std::min(y1, y0 + H - 1);
            xe = std::min(x1, x0 + W - 1);
        }

        for (int y = ys; y <= ye; ++y) {
            int dy = y;
            if (doWrap) { dy = ((y % H) + H) % H; }
            float ty = (y - (cy - halfH)) / (2 * halfH) * (TH - 1);
            int tyi = std::clamp((int)ty, 0, TH - 1);

            for (int x = xs; x <= xe; ++x) {
                int dx = x;
                if (doWrap) { dx = ((x % W) + W) % W; }
                float tx = (x - (cx - halfW)) / (2 * halfW) * (TW - 1);
                int txi = std::clamp((int)tx, 0, TW - 1);

                const float* tp = &tex[((size_t)tyi * TW + txi) * 3];
                if (tp[0] <= 0.f && tp[1] <= 0.f && tp[2] <= 0.f) continue;
                float sr = tp[0] * cr, sg = tp[1] * cg, sb = tp[2] * cb;

                float* dp = cur.at(dx, dy);
                switch (bm) {
                    case 0: dp[0] = sr; dp[1] = sg; dp[2] = sb; break;
                    default:
                    case 1: dp[0] = std::min(1.f, dp[0] + sr);
                            dp[1] = std::min(1.f, dp[1] + sg);
                            dp[2] = std::min(1.f, dp[2] + sb); break;
                    case 2: dp[0] = std::max(dp[0], sr);
                            dp[1] = std::max(dp[1], sg);
                            dp[2] = std::max(dp[2], sb); break;
                    case 3: dp[0] = (dp[0] + sr) * 0.5f;
                            dp[1] = (dp[1] + sg) * 0.5f;
                            dp[2] = (dp[2] + sb) * 0.5f; break;
                }
            }
        }
    }
}

// ---------------------------------------------------------------------------
//  GlobalVariablesEffect
// ---------------------------------------------------------------------------
GlobalVariablesEffect::GlobalVariablesEffect()
{
    // default: publish a beat-decaying pulse other effects can read
    _sInit = "reg00=0;";
    _sBeat = "reg00=1;";
    _sFrame = "reg00=reg00*0.92;";
}

void GlobalVariablesEffect::bindVars(eel::VM& vm)
{
    _w = vm.var("w");
    _h = vm.var("h");
}

void GlobalVariablesEffect::render(Framebuffer& cur, const Framebuffer& /*prev*/,
                                   const VizFrame& a, const EffectContext& /*ctx*/)
{
    ensureCompiled();
    *_w = cur.w; *_h = cur.h;
    runFrameScripts(a);
}

} // namespace viz
