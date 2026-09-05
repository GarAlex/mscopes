//
// LineMode.h — AVS's global line/dot blend state.
//
// vis_avs kept one process-wide `g_line_blend_mode` that every scope and dot
// renderer consulted through BLEND_LINE(): Superscope, Simple, Ring, the
// star/spin renderers, the dot fields, Moving Particle. Its default is
// REPLACE, opaque, 1 px; the "Set Render Mode" component overwrites it for
// everything rendered after it (and, being global, for the following frames
// until changed). We reproduce that as a shared state reset per preset load
// (AVS itself leaked it across presets — a quirk nobody wants back).
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

// Write one pixel through a blend mode (bounds-checked).
inline void blendPixel(Framebuffer& fb, int x, int y, float r, float g, float b,
                       int mode, float alpha)
{
    if (x < 0 || y < 0 || x >= fb.w || y >= fb.h) return;
    float* p = fb.at(x, y);
    const float s[3] = {r, g, b};
    for (int c = 0; c < 3; ++c) {
        float d = p[c], v;
        switch (mode) {
            case 1: v = std::min(1.f, d + s[c]); break;
            case 2: v = std::max(d, s[c]); break;
            case 3: v = (d + s[c]) * 0.5f; break;
            case 4: v = std::max(0.f, d - s[c]); break;
            case 5: v = std::max(0.f, s[c] - d); break;
            case 6: v = d * s[c]; break;
            case 7: v = d + (s[c] - d) * alpha; break;
            case 8: {
                int a = (int)std::lround(std::min(1.f, std::max(0.f, d)) * 255.f);
                int bb = (int)std::lround(std::min(1.f, std::max(0.f, s[c])) * 255.f);
                v = (float)(a ^ bb) / 255.f;
                break;
            }
            case 9: v = std::min(d, s[c]); break;
            default: v = s[c]; break;
        }
        p[c] = v;
    }
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

} // namespace viz
