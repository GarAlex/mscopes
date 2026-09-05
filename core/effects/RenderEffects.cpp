//
// RenderEffects.cpp — classic AVS render effects ported from the open-sourced
// vis_avs sources (Copyright 2005 Nullsoft, Inc., BSD-3-Clause). Algorithms
// follow the originals (see per-class notes in RenderEffects.h); adapted to
// float RGBA 0..1 framebuffers and normalized audio.
//
#include "RenderEffects.h"
#include "LineMode.h"
#include <cmath>
#include <cstring>
#include <algorithm>

namespace viz {

static constexpr float kPi = 3.14159265358979f;

// Simple HSV->RGB (h in [0,1)).
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

// Line in the global line mode (Set Render Mode), width included. Stands in
// for vis_avs linedraw.h line().
static void drawLine(Framebuffer& fb, int x0, int y0, int x1, int y1,
                     float r, float g, float b) {
    drawLineMode(fb, x0, y0, x1, y1, r, g, b);
}

// Scanline triangle fill in the global line mode (stands in for E_BassSpin::render_triangle).
static void fillTriangle(Framebuffer& fb, float x0, float y0, float x1, float y1,
                         float x2, float y2, float r, float g, float b) {
    if (y1 < y0) { std::swap(x0, x1); std::swap(y0, y1); }
    if (y2 < y0) { std::swap(x0, x2); std::swap(y0, y2); }
    if (y2 < y1) { std::swap(x1, x2); std::swap(y1, y2); }
    int yStart = std::max(0, (int)std::ceil(y0));
    int yEnd   = std::min(fb.h - 1, (int)std::floor(y2));
    for (int y = yStart; y <= yEnd; ++y) {
        float fy = (float)y;
        // long edge x
        float t02 = (y2 - y0) > 1e-6f ? (fy - y0) / (y2 - y0) : 0.f;
        float xa = x0 + (x2 - x0) * t02;
        float xb;
        if (fy < y1) {
            float t01 = (y1 - y0) > 1e-6f ? (fy - y0) / (y1 - y0) : 0.f;
            xb = x0 + (x1 - x0) * t01;
        } else {
            float t12 = (y2 - y1) > 1e-6f ? (fy - y1) / (y2 - y1) : 0.f;
            xb = x1 + (x2 - x1) * t12;
        }
        if (xb < xa) std::swap(xa, xb);
        int xs = std::max(0, (int)std::ceil(xa));
        int xe = std::min(fb.w - 1, (int)std::floor(xb));
        for (int x = xs; x <= xe; ++x) putLinePixel(fb, x, y, r, g, b);
    }
}

// 4x4 matrix helpers ported from vis_avs matrix.cpp (BSD-3). Row-major, the
// same axis convention (m: 1=x, 2=y, 3=z) as the original.
static void matrixRotate(float matrix[16], int m, float deg) {
    deg *= kPi / 180.0f;
    std::memset(matrix, 0, sizeof(float) * 16);
    matrix[((m - 1) << 2) + m - 1] = matrix[15] = 1.0f;
    int m1 = (m % 3);
    int m2 = ((m1 + 1) % 3);
    float c = std::cos(deg), s = std::sin(deg);
    matrix[(m1 << 2) + m1] = c;
    matrix[(m1 << 2) + m2] = s;
    matrix[(m2 << 2) + m2] = c;
    matrix[(m2 << 2) + m1] = -s;
}

static void matrixTranslate(float m[16], float x, float y, float z) {
    std::memset(m, 0, sizeof(float) * 16);
    m[0] = m[4 + 1] = m[8 + 2] = m[12 + 3] = 1.0f;
    m[0 + 3] = x;
    m[4 + 3] = y;
    m[8 + 3] = z;
}

static void matrixMultiply(float* dest, const float src[16]) {
    float temp[16];
    std::memcpy(temp, dest, sizeof(float) * 16);
    for (int i = 0; i < 16; i += 4) {
        for (int j = 0; j < 4; ++j)
            *dest++ = src[i + 0] * temp[(0 << 2) + j] + src[i + 1] * temp[(1 << 2) + j]
                    + src[i + 2] * temp[(2 << 2) + j] + src[i + 3] * temp[(3 << 2) + j];
    }
}

static void matrixApply(const float* m, float x, float y, float z,
                        float* outx, float* outy, float* outz) {
    *outx = x * m[0] + y * m[1] + z * m[2] + m[3];
    *outy = x * m[4] + y * m[5] + z * m[6] + m[7];
    *outz = x * m[8] + y * m[9] + z * m[10] + m[11];
}

// Build the original 5-stop, 4x16-step gradient LUT (init_color_map from
// e_dotplane.cpp / e_dotfountain.cpp), colors given as 0xRRGGBB.
static void buildColorMap5(const uint32_t colors[5], float map[64][3]) {
    for (int t = 0; t < 4; ++t) {
        uint32_t c1 = colors[t], c2 = colors[t + 1];
        float r  = (float)((c1 >> 16) & 255), g  = (float)((c1 >> 8) & 255), b  = (float)(c1 & 255);
        float dr = ((float)((c2 >> 16) & 255) - r) / 16.f;
        float dg = ((float)((c2 >> 8)  & 255) - g) / 16.f;
        float db = ((float)( c2        & 255) - b) / 16.f;
        for (int x = 0; x < 16; ++x) {
            map[t * 16 + x][0] = r / 255.f;
            map[t * 16 + x][1] = g / 255.f;
            map[t * 16 + x][2] = b / 255.f;
            r += dr; g += dg; b += db;
        }
    }
}

// Linear interpolation into a waveform channel, result -1..1.
static float sampleWave(const VizFrame& a, int ch, float pos) {
    pos = std::clamp(pos, 0.f, (float)(kWaveformSamples - 1));
    int i0 = (int)pos;
    int i1 = std::min(i0 + 1, kWaveformSamples - 1);
    float f = pos - i0;
    return a.waveform[ch][i0] * (1.f - f) + a.waveform[ch][i1] * f;
}

// Linear interpolation into a spectrum channel, result 0..1.
static float sampleSpec(const VizFrame& a, int ch, float pos) {
    pos = std::clamp(pos, 0.f, (float)(kSpectrumBins - 1));
    int i0 = (int)pos;
    int i1 = std::min(i0 + 1, kSpectrumBins - 1);
    float f = pos - i0;
    return a.spectrum[ch][i0] * (1.f - f) + a.spectrum[ch][i1] * f;
}

// ---------------------------------------------------------------------------
// Our take on AVS "Simple" (e_simple.cpp, BSD-3).
void SimpleScopeEffect::render(Framebuffer& cur, const Framebuffer& /*prev*/,
                               const VizFrame& a, const EffectContext& /*ctx*/)
{
    const int W = cur.w, H = cur.h;
    if (W <= 1 || H == 0) return;
    const int m   = std::clamp((int)mode, 0, 4);
    const int pos = std::clamp((int)position, 0, 2);   // 0 top, 1 center, 2 bottom

    const int nCh = std::max(1, std::min(2, a.numWaveformChannels ? a.numWaveformChannels : 2));
    for (int ch = 0; ch < nCh; ++ch) {
        float r, g, b;
        hsv(ch == 0 ? hueLeft : hueRight, 0.85f, gain, r, g, b);
        const int yOff = ch == 1 ? 3 : 0;   // stereo channels drawn slightly offset

        if (m <= 2) {
            // Oscilloscope. Band is half the screen height, like the original's
            // yscale = h/2/256 with byte samples. yh is the band's top edge.
            const int yh = (pos == 0) ? 0 : (pos == 2) ? H / 2 : H / 4;
            auto waveY = [&](float t) {
                float v01 = sampleWave(a, ch, t * (kWaveformSamples - 1)) * 0.5f + 0.5f;
                return yh + yOff + (int)(v01 * (H / 2));
            };
            if (m == 1) {              // dots
                for (int x = 0; x < W; ++x)
                    putLinePixel(cur, x, waveY((float)x / (W - 1)), r, g, b);
            } else if (m == 0) {       // lines (orig: 288 segments; we span 512 samples)
                const int N = 288;
                int lx = 0, ly = waveY(0.f);
                for (int i = 1; i < N; ++i) {
                    int ox = i * W / (N - 1);
                    int oy = waveY((float)i / (N - 1));
                    drawLine(cur, lx, ly, ox, oy, r, g, b);
                    lx = ox; ly = oy;
                }
            } else {                   // solid: column from band center to sample
                const int ys = yh + yOff + H / 4;
                for (int x = 0; x < W; ++x)
                    drawLine(cur, x, ys - 1, x, waveY((float)x / (W - 1)), r, g, b);
            }
        } else {
            // Spectrum analyzer. Original uses the lower ~70% of the bins
            // (200 of 288); bars occupy half the height.
            const float binSpan = kSpectrumBins * 0.7f;
            // top: up from h/2; bottom: down from h/2; center: up from 3h/4.
            const int   base = (pos == 1) ? (3 * H / 4) : (H / 2);
            const float dir  = (pos == 2) ? 1.f : -1.f;
            auto specY = [&](float t) {
                float v = sampleSpec(a, ch, t * binSpan);
                return base + yOff + (int)(dir * v * (H / 2));
            };
            if (m == 4) {              // dots
                for (int x = 0; x < W; ++x)
                    putLinePixel(cur, x, specY((float)x / (W - 1)), r, g, b);
            } else {                   // lines (orig: 200 segments)
                const int N = 200;
                int lx = 0, ly = specY(0.f);
                for (int i = 1; i < N; ++i) {
                    int ox = i * W / (N - 1);
                    int oy = specY((float)i / (N - 1));
                    drawLine(cur, lx, ly, ox, oy, r, g, b);
                    lx = ox; ly = oy;
                }
            }
        }
    }
}

// ---------------------------------------------------------------------------
// Our take on AVS "Ring" (e_ring.cpp, BSD-3).
void RingEffect::render(Framebuffer& cur, const Framebuffer& /*prev*/,
                        const VizFrame& a, const EffectContext& /*ctx*/)
{
    const int W = cur.w, H = cur.h;
    if (W == 0 || H == 0) return;
    float r, g, b;
    hsv(hue, 0.85f, gain, r, g, b);

    const float sizePx = size * (float)std::min(W, H);
    const int cy = H / 2;
    const int p  = std::clamp((int)posX, 0, 2);
    const int cx = (p == 0) ? W / 4 : (p == 2) ? (W / 2 + W / 4) : W / 2;

    // 80 segments; samples mirror across the halves (q>40 -> 80-q) and the
    // original only taps the first 41 waveform values — we spread those taps
    // across our 512-sample buffer.
    auto sca = [&](int q) {
        int qq = q > 40 ? 80 - q : q;
        float v01 = sampleWave(a, 0, qq * (kWaveformSamples - 1) / 40.f) * 0.5f + 0.5f;
        return 0.1f + v01 * 0.9f;
    };

    const int passes = std::clamp((int)thickness, 1, 4);
    for (int pass = 0; pass < passes; ++pass) {
        const float rad = sizePx + pass;
        double ang = 0.0;
        float s0 = sca(0);
        int lx = cx + (int)(std::cos(ang) * rad * s0);
        int ly = cy + (int)(std::sin(ang) * rad * s0);
        for (int q = 1; q <= 80; ++q) {
            ang -= kPi * 2.0 / 80.0;
            float s = sca(q);
            int tx = cx + (int)(std::cos(ang) * rad * s);
            int ty = cy + (int)(std::sin(ang) * rad * s);
            drawLine(cur, tx, ty, lx, ly, r, g, b);
            lx = tx; ly = ty;
        }
    }
}

// ---------------------------------------------------------------------------
// Our take on AVS "Oscilloscope Star" (e_oscilloscopestar.cpp, BSD-3).
void OscStarEffect::render(Framebuffer& cur, const Framebuffer& /*prev*/,
                           const VizFrame& a, const EffectContext& /*ctx*/)
{
    const int W = cur.w, H = cur.h;
    if (W == 0 || H == 0) return;
    float r, g, b;
    hsv(hue, 0.85f, gain, r, g, b);

    const float sizePx = size * (float)std::min(W, H);
    const int cx = W / 2;
    int ii = 0;
    for (int q = 0; q < 5; ++q) {
        double s = std::sin(_rot + q * (kPi * 2.0 / 5.0));
        double c = std::cos(_rot + q * (kPi * 2.0 / 5.0));
        double p = 0.0;
        int lx = cx, ly = H / 2;
        int t = 64;
        double dp = sizePx / 64.0;
        double dfactor = 1.0 / 1024.0;
        while (t--) {
            // orig: ale = ((fa^128)-128) * dfactor * hw  (signed byte)
            double ale = a.waveform[0][ii & (kWaveformSamples - 1)] * 128.0 * dfactor * sizePx;
            ++ii;
            int x = cx + (int)(c * p) - (int)(s * ale);
            int y = H / 2 + (int)(s * p) + (int)(c * ale);
            drawLine(cur, x, y, lx, ly, r, g, b);
            lx = x; ly = y;
            p += dp;
            dfactor -= ((1.0 / 1024.0) - (1.0 / 128.0)) / 64.0;  // deflection grows toward the tip
        }
    }
    _rot += 0.01 * (double)rotSpeed;
    if (_rot >= kPi * 2) _rot -= kPi * 2;
    if (_rot < 0)        _rot += kPi * 2;
}

// ---------------------------------------------------------------------------
// Our take on AVS "Rotating Stars" (e_rotstar.cpp, BSD-3).
void RotStarEffect::render(Framebuffer& cur, const Framebuffer& /*prev*/,
                           const VizFrame& a, const EffectContext& /*ctx*/)
{
    const int W = cur.w, H = cur.h;
    if (W == 0 || H == 0) return;

    const int x = (int)(std::cos(_rot) * W / 4.0);
    const int y = (int)(std::sin(_rot) * H / 4.0);
    for (int c = 0; c < 2; ++c) {
        float cr, cg, cb;
        hsv(c == 0 ? hueLeft : hueRight, 0.85f, gain, cr, cg, cb);
        const int ch = std::min(c, std::max(0, a.numSpectrumChannels - 1));

        // Spectral peak pick over low bins 3..13 (byte scale like the original).
        float s = 0.f;
        for (int l = 3; l < 14; ++l) {
            float v = a.spectrum[ch][l] * 255.f;
            if (v > s && v > a.spectrum[ch][l + 1] * 255.f + 4.f
                      && v > a.spectrum[ch][l - 1] * 255.f + 4.f)
                s = v;
        }

        int ax = x, by = y;
        if (c == 1) { ax = -ax; by = -by; }

        const double vw = W / 8.0 * (s + 9.f) / 88.0;
        const double vh = H / 8.0 * (s + 9.f) / 88.0;

        double r2 = -_rot;
        int lx = W / 2 + ax + (int)(std::cos(r2) * vw);
        int ly = H / 2 + by + (int)(std::sin(r2) * vh);
        r2 += kPi * 4.0 / 5.0;
        for (int t = 0; t < 5; ++t) {
            int nx = (int)(std::cos(r2) * vw) + W / 2 + ax;
            int ny = (int)(std::sin(r2) * vh) + H / 2 + by;
            r2 += kPi * 4.0 / 5.0;
            drawLine(cur, lx, ly, nx, ny, cr, cg, cb);
            lx = nx; ly = ny;
        }
    }
    _rot += (double)speed;   // orig: fixed +0.1/frame
}

// ---------------------------------------------------------------------------
// Our take on AVS "Bass Spin" (e_bassspin.cpp, BSD-3).
void BassSpinEffect::render(Framebuffer& cur, const Framebuffer& /*prev*/,
                            const VizFrame& a, const EffectContext& /*ctx*/)
{
    const int W = cur.w, H = cur.h;
    if (W == 0 || H == 0) return;
    const bool filled = (int)mode >= 1;

    for (int t = 0; t < 2; ++t) {
        const int ch = std::min(t, std::max(0, a.numSpectrumChannels - 1));
        float cr, cg, cb;
        hsv(t == 0 ? hueLeft : hueRight, 0.85f, gain, cr, cg, cb);

        const int screenSize = std::min(H / 2, (W * 3) / 8);
        const int cx = (t == 0) ? W / 2 - screenSize / 2 : W / 2 + screenSize / 2;

        // Summed low-spectrum energy through the original's AGC.
        float d = 0.f;
        for (int i = 0; i < 44; ++i) d += a.spectrum[ch][i] * 255.f;
        float amp = (d * 512.f) / (_lastA + 30.f * 256.f);
        _lastA = d;                    // shared across channels, as in the original
        if (amp > 255.f) amp = 255.f;

        _v[t] = 0.7 * (std::max(amp - 104.f, 12.f) / 96.0) + 0.3 * _v[t];
        _rv[t] += kPi / 6.0 * _v[t] * _dir[t];

        const double sizeF = (double)screenSize * amp / 256.0;
        const int xp = (int)(std::cos(_rv[t]) * sizeF);
        const int yp = (int)(std::sin(_rv[t]) * sizeF);

        if (!filled) {
            if (_hasLast[t])
                drawLine(cur, _lx[0][t], _ly[0][t], xp + cx, yp + H / 2, cr, cg, cb);
            drawLine(cur, cx, H / 2, cx + xp, H / 2 + yp, cr, cg, cb);
            if (_hasLast[t])
                drawLine(cur, _lx[1][t], _ly[1][t], cx - xp, H / 2 - yp, cr, cg, cb);
            drawLine(cur, cx, H / 2, cx - xp, H / 2 - yp, cr, cg, cb);
        } else {
            if (_hasLast[t]) {
                fillTriangle(cur, (float)cx, H * 0.5f,
                             (float)_lx[0][t], (float)_ly[0][t],
                             (float)(xp + cx), (float)(yp + H / 2),
                             cr * 0.5f, cg * 0.5f, cb * 0.5f);
                fillTriangle(cur, (float)cx, H * 0.5f,
                             (float)_lx[1][t], (float)_ly[1][t],
                             (float)(cx - xp), (float)(H / 2 - yp),
                             cr * 0.5f, cg * 0.5f, cb * 0.5f);
            }
        }
        _lx[0][t] = xp + cx;  _ly[0][t] = yp + H / 2;
        _lx[1][t] = cx - xp;  _ly[1][t] = H / 2 - yp;
        _hasLast[t] = true;
    }
}

// ---------------------------------------------------------------------------
// Our take on AVS "Dot Grid" (e_dotgrid.cpp, BSD-3).
void DotGridEffect::render(Framebuffer& cur, const Framebuffer& /*prev*/,
                           const VizFrame& /*a*/, const EffectContext& /*ctx*/)
{
    const int W = cur.w, H = cur.h;
    if (W == 0 || H == 0) return;
    const int sp = std::max(2, (int)spacing);

    float r, g, b;
    hsv(hue, 0.85f, gain, r, g, b);

    // Wrap accumulators into [0, spacing) like the original's fixed-point xp/yp.
    auto wrap = [sp](float v) {
        float m = std::fmod(v, (float)sp);
        return m < 0 ? m + sp : m;
    };
    _xp = wrap(_xp);
    _yp = wrap(_yp);

    const int sx = (int)_xp, sy = (int)_yp;
    for (int y = sy; y < H; y += sp)
        for (int x = sx; x < W; x += sp)
            putLinePixel(cur, x, y, r, g, b);

    _xp += speedX;
    _yp += speedY;
}

// ---------------------------------------------------------------------------
// Our take on AVS "Dot Plane" (e_dotplane.cpp, BSD-3).
DotPlaneEffect::DotPlaneEffect()
{
    static const uint32_t kColors[5] = {   // original default gradient
        0x1c6b18, 0xff0a23, 0x2a1d74, 0x9036d9, 0x6b88ff};
    buildColorMap5(kColors, _map);
}

void DotPlaneEffect::render(Framebuffer& cur, const Framebuffer& /*prev*/,
                            const VizFrame& a, const EffectContext& /*ctx*/)
{
    const int W = cur.w, H = cur.h;
    if (W == 0 || H == 0) return;

    float transform[16], transform2[16];
    matrixRotate(transform, 2, (float)_rotation);
    matrixRotate(transform2, 1, angle);
    matrixMultiply(transform, transform2);
    matrixTranslate(transform2, 0.0f, -20.0f, 400.0f);
    matrixMultiply(transform, transform2);

    float tmpLine[kGrid];
    std::memcpy(tmpLine, &_height[0], sizeof(float) * kGrid);

    // Scroll the plane: each row inherits the previous one; the edge row is
    // refreshed from the spectrum (max of 3 adjacent bins, byte scale).
    int line = (kGrid - 2) * kGrid;
    for (int yPos = 0; yPos < kGrid; ++yPos, line -= kGrid) {
        float* curH  = &_height[line];
        float* nextH = &_height[line + kGrid];
        float* curD  = &_delta[line];
        float* nextD = &_delta[line + kGrid];
        auto*  prevC = &_color[line];
        auto*  nextC = &_color[line + kGrid];
        if (yPos == kGrid - 1) {
            curH = tmpLine;   // old row 0, saved before it was overwritten
            for (int x = 0; x < kGrid; ++x) {
                float audio = std::max({a.spectrum[0][x * 3],
                                        a.spectrum[0][x * 3 + 1],
                                        a.spectrum[0][x * 3 + 2]}) * 255.f;
                nextH[x] = audio;
                int ci = std::min((int)audio / 4, kMapSize - 1);
                nextC[x][0] = _map[ci][0]; nextC[x][1] = _map[ci][1]; nextC[x][2] = _map[ci][2];
                nextD[x] = (nextH[x] - curH[x]) / 90.0f;
            }
        } else {
            for (int x = 0; x < kGrid; ++x) {
                nextH[x] = curH[x] + curD[x];
                if (nextH[x] < 0.0f) nextH[x] = 0.0f;
                nextD[x] = curD[x] - 0.15f * (nextH[x] / 255.0f);
                nextC[x][0] = prevC[x][0]; nextC[x][1] = prevC[x][1]; nextC[x][2] = prevC[x][2];
            }
        }
    }

    float zoom  = (float)W * 440.0f / 640.0f;
    float zoom2 = (float)H * 440.0f / 480.0f;
    if (zoom2 < zoom) zoom = zoom2;

    // Painter's order flips with the view rotation, exactly as the original.
    for (int yPos = 0; yPos < kGrid; ++yPos) {
        int gridStart = (_rotation < 90.0 || _rotation > 270.0) ? kGrid - yPos - 1 : yPos;
        float gridStep = 350.0f / (float)kGrid;
        float curY = -(kGrid * 0.5f) * gridStep;
        float curX = ((float)gridStart - kGrid * 0.5f) * gridStep;
        int idx = gridStart * kGrid;
        int direction = 1;
        if (_rotation < 180.0) {
            direction = -1;
            gridStep = -gridStep;
            curY = -curY + gridStep;
            idx += kGrid - 1;
        }
        for (int xPos = 0; xPos < kGrid; ++xPos) {
            float x, y, z;
            matrixApply(transform, curY, 64.0f - _height[idx], curX, &x, &y, &z);
            if (z > 0.0000001f) {
                z = zoom / z;
                int sx = (int)(x * z) + W / 2;
                int sy = (int)(y * z) + H / 2;
                putLinePixel(cur, sx, sy, _color[idx][0] * gain, _color[idx][1] * gain,
                               _color[idx][2] * gain);
            }
            curY += gridStep;
            idx += direction;
        }
    }

    _rotation += (double)rotSpeed / 5.0;
    if (_rotation >= 360.0) _rotation -= 360.0;
    if (_rotation < 0.0)    _rotation += 360.0;
}

// ---------------------------------------------------------------------------
// Our take on AVS "Dot Fountain" (e_dotfountain.cpp, BSD-3).
DotFountainEffect::DotFountainEffect()
{
    static const uint32_t kColors[5] = {   // original default gradient
        0x1c6b18, 0xff0a23, 0x2a1d74, 0x9036d9, 0x6b88ff};
    buildColorMap5(kColors, _map);
    _points.assign((size_t)kRotHeight * kRotDiv, Point{});
}

void DotFountainEffect::render(Framebuffer& cur, const Framebuffer& /*prev*/,
                               const VizFrame& a, const EffectContext& /*ctx*/)
{
    const int W = cur.w, H = cur.h;
    if (W == 0 || H == 0) return;

    float transform[16], transform2[16];
    matrixRotate(transform, 2, (float)_rotation);
    matrixRotate(transform2, 1, angle);
    matrixMultiply(transform, transform2);
    matrixTranslate(transform2, 0.0f, -20.0f, 400.0f);
    matrixMultiply(transform, transform2);

    // Age every point: gravity (+0.05/frame), outward radial acceleration.
    for (int gen = kRotHeight - 2; gen >= 0; --gen) {
        const float accelRadius = 1.3f / ((float)gen + 100.f);
        Point* prev = &_points[(size_t)gen * kRotDiv];
        Point* next = &_points[(size_t)(gen + 1) * kRotDiv];
        for (int i = 0; i < kRotDiv; ++i) {
            next[i] = prev[i];
            next[i].radius  += next[i].dRadius;
            next[i].dHeight += 0.05f;
            next[i].dRadius += accelRadius;
            next[i].height  += next[i].dHeight;
        }
    }

    // Launch a new ring from the waveform (boosted on beat, like is_beat*128).
    Point* ring = &_points[0];
    for (int i = 0; i < kRotDiv; ++i) {
        // orig: audio_sample = byte^128 (0..255 around 128)
        int sample = (int)(a.waveform[0][i] * 127.f) + 128;
        int audio = sample * 5 / 4 - 64 + (a.beat ? 128 : 0);
        if (audio > 255) audio = 255;
        Point& p = ring[i];
        p.radius = 1.0f;
        p.height = 250.f;
        float dr = (float)std::abs(audio) / 200.0f + 1.0f;
        p.dHeight = -dr * 2.8f;     // initial upward speed
        int ci = std::clamp(audio / 4, 0, kMapSize - 1);
        p.r = _map[ci][0]; p.g = _map[ci][1]; p.b = _map[ci][2];
        float ang = (float)i * kPi * 2.0f / kRotDiv;
        p.ax = std::sin(ang);
        p.ay = std::cos(ang);
        p.dRadius = 0.0f;
    }

    // Project and splat.
    float zoom  = (float)W * 440.0f / 640.0f;
    float zoom2 = (float)H * 440.0f / 480.0f;
    if (zoom2 < zoom) zoom = zoom2;
    for (const Point& p : _points) {
        float x, y, z;
        matrixApply(transform, p.ax * p.radius, p.height, p.ay * p.radius, &x, &y, &z);
        if (z > 0.0000001f) {
            z = zoom / z;
            int sx = (int)(x * z) + W / 2;
            int sy = (int)(y * z) + H / 2;
            putLinePixel(cur, sx, sy, p.r * gain, p.g * gain, p.b * gain);
        }
    }

    _rotation += (double)rotSpeed / 5.0;
    if (_rotation >= 360.0) _rotation -= 360.0;
    if (_rotation < 0.0)    _rotation += 360.0;
}

// ---------------------------------------------------------------------------
// Our take on AVS "Timescope" (e_timescope.cpp, BSD-3).
void TimescopeEffect::render(Framebuffer& cur, const Framebuffer& prev,
                             const VizFrame& a, const EffectContext& /*ctx*/)
{
    const int W = cur.w, H = cur.h;
    if (W <= 1 || H == 0 || prev.w != W || prev.h != H) return;

    // Scroll: previous frame shifted left one pixel is our base (the original
    // swept a stationary write cursor instead of scrolling).
    for (int y = 0; y < H; ++y) {
        const float* src = prev.at(1, y);
        float* dst = cur.at(0, y);
        std::memcpy(dst, src, sizeof(float) * 4 * (size_t)(W - 1));
    }

    // Newest column at the right edge: per-row band intensity tinted by hue.
    float r, g, b;
    hsv(hue, 0.85f, 1.f, r, g, b);
    const int nBands = std::clamp((int)bands, 1, kSpectrumBins);
    for (int y = 0; y < H; ++y) {
        int band = std::min((y * nBands) / H, kSpectrumBins - 1);
        float v = std::min(1.f, a.spectrum[0][band] * gain);
        float* d = cur.at(W - 1, y);
        d[0] = r * v; d[1] = g * v; d[2] = b * v; d[3] = 1.f;
    }
}

} // namespace viz

// ---------------------------------------------------------------------------
//  MovingParticleEffect — see RenderEffects.h.
//  Our take on AVS Moving Particle (e_movingparticle.cpp, BSD-3).
// ---------------------------------------------------------------------------
namespace viz {

float MovingParticleEffect::frand()
{
    _rng ^= _rng << 13; _rng ^= _rng >> 17; _rng ^= _rng << 5;
    return (_rng & 0xFFFFFF) / (float)0x1000000;
}

void MovingParticleEffect::render(Framebuffer& cur, const Framebuffer& /*prev*/,
                                  const VizFrame& a, const EffectContext& /*ctx*/)
{
    const int W = cur.w, H = cur.h;
    if (W == 0 || H == 0) return;
    const double ss = std::min(H / 2, (W * 3) / 8);

    if (a.beat) {
        // original: ((rand()%33)-16)/48.0
        _c[0] = (std::floor(frand() * 33.f) - 16.f) / 48.0;
        _c[1] = (std::floor(frand() * 33.f) - 16.f) / 48.0;
        if (onBeatSizeChange > 0.5f) _curSize = onBeatSize;
    }

    _v[0] -= 0.004 * (_p[0] - _c[0]);
    _v[1] -= 0.004 * (_p[1] - _c[1]);
    _p[0] += _v[0];
    _p[1] += _v[1];
    _v[0] *= 0.991;
    _v[1] *= 0.991;

    const double reach = ss * (distance / 32.0);
    const int xp = (int)(_p[0] * reach) + W / 2;
    const int yp = (int)(_p[1] * reach) + H / 2;

    int sz = (int)_curSize;
    _curSize = (_curSize + size) * 0.5f;    // ease back to base size
    sz = std::min(sz, 128);

    const int bm = (int)blendMode;
    auto put = [&](int x, int y) {
        if (x < 0 || y < 0 || x >= W || y >= H) return;
        float* d = cur.at(x, y);
        switch (bm) {
            case 0:  d[0] = colR; d[1] = colG; d[2] = colB; break;              // replace
            case 2:  d[0] = (d[0]+colR)*0.5f; d[1] = (d[1]+colG)*0.5f;
                     d[2] = (d[2]+colB)*0.5f; break;                            // 50/50
            default: d[0] = std::min(1.f, d[0]+colR);
                     d[1] = std::min(1.f, d[1]+colG);
                     d[2] = std::min(1.f, d[2]+colB); break;                    // additive
        }
    };

    if (sz <= 1) { put(xp, yp); return; }

    // filled circle, scanline by scanline (original's exact shape math)
    const double md = sz * sz * 0.25;
    const int y0 = yp - sz / 2;
    for (int y = 0; y < sz; ++y) {
        const int yy = y0 + y;
        if (yy < 0 || yy >= H) continue;
        double yd = y - sz * 0.5;
        double l = std::sqrt(std::max(0.0, md - yd * yd));
        int xs = std::max(1, (int)(l + 0.99));
        for (int x = std::max(0, xp - xs); x < std::min(W, xp + xs); ++x)
            put(x, yy);
    }
}

} // namespace viz
