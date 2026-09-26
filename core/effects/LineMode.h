//
// LineMode.h — AVS's global line/dot blend state, and how lines and dots
// are rasterized.
//
// vis_avs kept one process-wide `g_line_blend_mode` that every scope and dot
// renderer consulted through BLEND_LINE(): Superscope, Simple, Ring, the
// star/spin renderers, the dot fields, Moving Particle. Its default is
// REPLACE, opaque, 1 px; the "Set Render Mode" component overwrites it for
// everything rendered after it (and, being global, for the following frames
// until changed). We reproduce that as a shared state reset per preset load
// (AVS itself leaked it across presets — a quirk nobody wants back).
//
// Draw quality (DrawQuality) chooses the rasterizer. Authentic is exactly
// AVS: hard one-pixel Bresenham lines and single-pixel dots at truncated
// integer positions. Smooth is coverage anti-aliasing at sub-pixel
// positions, applied as each pixel is written — so the smooth pixels are
// what feedback effects carry forward, and trails stay smooth too. The host
// sets it per frame (Picture ▸ Quality: Smooth lines, Classic presets).
//
#pragma once
#include "Framebuffer.h"
#include <algorithm>
#include <cmath>

namespace viz {

struct LineMode {
    // 0 replace, 1 additive, 2 maximum, 3 50/50, 4 dest-src, 5 src-dest,
    // 6 multiply, 7 adjustable (alpha), 8 xor, 9 minimum
    int   blend = 0;
    float alpha = 1.f;
    int   width = 1;          // 1..255 pixels
};

LineMode& lineMode();         // the shared state (UtilEffects.cpp)

struct DrawQuality {
    bool  smooth = false;     // anti-aliased, sub-pixel lines and dots
    float widthScale = 1.f;   // render pixels per "classic" pixel: line widths and dots scale by it
};

DrawQuality& drawQuality();   // shared state, set by the host (UtilEffects.cpp)

// One channel through a blend mode.
inline float blendChannel(int mode, float d, float s, float alpha)
{
    switch (mode) {
        case 1: return std::min(1.f, d + s);
        case 2: return std::max(d, s);
        case 3: return (d + s) * 0.5f;
        case 4: return std::max(0.f, d - s);
        case 5: return std::max(0.f, s - d);
        case 6: return d * s;
        case 7: return d + (s - d) * alpha;
        case 8: {
            int a = (int)std::lround(std::min(1.f, std::max(0.f, d)) * 255.f);
            int b = (int)std::lround(std::min(1.f, std::max(0.f, s)) * 255.f);
            return (float)(a ^ b) / 255.f;
        }
        case 9: return std::min(d, s);
        default: return s;
    }
}

// Write one pixel through a blend mode (bounds-checked).
inline void blendPixel(Framebuffer& fb, int x, int y, float r, float g, float b,
                       int mode, float alpha)
{
    if (x < 0 || y < 0 || x >= fb.w || y >= fb.h) return;
    float* p = fb.at(x, y);
    p[0] = blendChannel(mode, p[0], r, alpha);
    p[1] = blendChannel(mode, p[1], g, alpha);
    p[2] = blendChannel(mode, p[2], b, alpha);
    p[3] = 1.f;
}

// The same with partial coverage (0..1): the fully blended value, mixed in
// by coverage. Works uniformly for every blend mode.
inline void blendPixelCoverage(Framebuffer& fb, int x, int y, float r, float g, float b,
                               int mode, float alpha, float cov)
{
    if (cov <= 0.f || x < 0 || y < 0 || x >= fb.w || y >= fb.h) return;
    if (cov > 1.f) cov = 1.f;
    float* p = fb.at(x, y);
    const float s[3] = {r, g, b};
    for (int c = 0; c < 3; ++c) p[c] += (blendChannel(mode, p[c], s[c], alpha) - p[c]) * cov;
    p[3] = 1.f;
}

// The BLEND_LINE analogue: one pixel in the current global mode.
inline void putLinePixel(Framebuffer& fb, int x, int y, float r, float g, float b)
{
    const LineMode& m = lineMode();
    blendPixel(fb, x, y, r, g, b, m.blend, m.alpha);
}

// Bresenham line in the current global blend mode; `width` 0 = the global
// width (extra parallel lines along the minor axis, as vis_avs linedraw.h
// does). Superscope passes its own `linesize` instead, as the original did.
// UtilEffects.cpp.
void drawLineMode(Framebuffer& fb, int x0, int y0, int x1, int y1, float r, float g, float b,
                  int width = 0);

// --- quality-aware entry points (float positions) --------------------------
// Pixel x covers [x, x+1): a float position p lands in pixel (int)p when
// Authentic, which keeps the exact truncation the integer callers had.
//
// A line in the current global mode. Authentic: drawLineMode on truncated
// endpoints. Smooth: an anti-aliased segment of width*widthScale.
//
// Polylines: a segment owns the stretch between its points; `capStart` /
// `capEnd` add a round cap beyond them. Continuation segments pass
// capStart = sharpTurn(...) and capEnd only on the last one, so straight
// runs and gentle bends get no double light (no "beads" when additive)
// where segments meet, and sharp corners get a round join.
void drawLineQ(Framebuffer& fb, float x0, float y0, float x1, float y1,
               float r, float g, float b, int width = 0,
               bool capStart = true, bool capEnd = true);

// True when a path turns by more than about 35° at b going a -> b -> c.
inline bool sharpTurn(float ax, float ay, float bx, float by, float cx, float cy)
{
    const float ux = bx - ax, uy = by - ay, vx = cx - bx, vy = cy - by;
    const float lu = ux * ux + uy * uy, lv = vx * vx + vy * vy;
    if (lu < 1e-8f || lv < 1e-8f) return true;
    return (ux * vx + uy * vy) / std::sqrt(lu * lv) < 0.82f;
}

// A dot in the current global mode. Authentic: one pixel at the truncated
// position. Smooth: a one-pixel footprint split over the pixels it covers
// (sub-pixel motion), or an anti-aliased disc when widthScale makes it wider.
void putDotQ(Framebuffer& fb, float x, float y, float r, float g, float b);

// The smooth primitives with an explicit blend mode (effects that don't use
// the global line mode, e.g. additive built-ins). halfWidth in pixels.
void drawSegmentAA(Framebuffer& fb, float x0, float y0, float x1, float y1, float halfWidth,
                   float r, float g, float b, int mode, float alpha,
                   bool capStart = true, bool capEnd = true);
void plotDotAA(Framebuffer& fb, float x, float y, float radius,
               float r, float g, float b, int mode, float alpha);

} // namespace viz
