//
// UtilEffects.cpp
//
#include "UtilEffects.h"
#include "LineMode.h"

namespace viz {

static void fillOrBlend(Framebuffer& cur, float r, float g, float b, bool blend50)
{
    const int n = cur.w * cur.h;
    float* px = cur.px.data();
    if (blend50) {
        for (int i = 0; i < n; ++i, px += 4) {
            px[0] = (px[0] + r) * 0.5f;
            px[1] = (px[1] + g) * 0.5f;
            px[2] = (px[2] + b) * 0.5f;
        }
    } else {
        for (int i = 0; i < n; ++i, px += 4) {
            px[0] = r; px[1] = g; px[2] = b; px[3] = 1.f;
        }
    }
}

void ClearScreenEffect::render(Framebuffer& cur, const Framebuffer& /*prev*/,
                               const VizFrame& /*a*/, const EffectContext& ctx)
{
    if (onlyFirst > 0.5f) {
        if (_hasCleared) return;
        _hasCleared = true;
        fillOrBlend(cur, r, g, b, blend > 0.5f);
        return;
    }
    int n = (int)everyN; if (n < 1) n = 1;
    if (ctx.frame % (uint64_t)n) return;
    fillOrBlend(cur, r, g, b, blend > 0.5f);
}

void OnBeatClearEffect::render(Framebuffer& cur, const Framebuffer& /*prev*/,
                               const VizFrame& a, const EffectContext& /*ctx*/)
{
    if (!a.beat) return;
    int n = (int)everyNBeats; if (n < 1) n = 1;
    if (++_beatCount < n) return;
    _beatCount = 0;
    fillOrBlend(cur, r, g, b, blend > 0.5f);
}

// ---------------------------------------------------------------------------
//  Buffer Save + the global buffer pool
// ---------------------------------------------------------------------------
Framebuffer& globalBuffer(int index)
{
    static Framebuffer pool[8];
    return pool[std::clamp(index, 0, 7)];
}

LineMode& lineMode()
{
    static LineMode m;
    return m;
}

DrawQuality& drawQuality()
{
    static DrawQuality q;
    return q;
}

// ---------------------------------------------------------------------------
//  Smooth rasterizer (DrawQuality::smooth)
// ---------------------------------------------------------------------------
// Coverage anti-aliasing: a pixel's coverage is how far its centre lies
// inside the shape, with a one-pixel falloff at the edge —
// clamp(halfWidth + 0.5 - distance, 0, 1). For a line that integrates to its
// width per unit length whatever the angle or sub-pixel offset, so a smooth
// line carries the same light as the hard line it replaces. Pixel centres
// are at integer + 0.5 (pixel x covers [x, x+1)).

// Coverage blend specialised per mode: the mode is fixed for a whole
// segment, so the per-pixel switch folds away. Callers clip to the frame.
using CovBlendFn = void (*)(float* p, const float s[3], float alpha, float cov);
template <int M>
static void covBlend(float* p, const float s[3], float alpha, float cov)
{
    if (cov > 1.f) cov = 1.f;
    p[0] += (blendChannel(M, p[0], s[0], alpha) - p[0]) * cov;
    p[1] += (blendChannel(M, p[1], s[1], alpha) - p[1]) * cov;
    p[2] += (blendChannel(M, p[2], s[2], alpha) - p[2]) * cov;
    p[3] = 1.f;
}
static CovBlendFn covBlendFor(int mode)
{
    switch (mode) {
        case 1: return covBlend<1>; case 2: return covBlend<2>; case 3: return covBlend<3>;
        case 4: return covBlend<4>; case 5: return covBlend<5>; case 6: return covBlend<6>;
        case 7: return covBlend<7>; case 8: return covBlend<8>; case 9: return covBlend<9>;
        default: return covBlend<0>;
    }
}

// Additive with coverage, HDR: fb gets exactly what mode 1 would write
// (clipped at white), the part that clipped goes to the overflow buffer.
static inline void addCoverageHdr(Framebuffer& fb, Framebuffer& ov, int x, int y,
                                  const float s[3], float cov)
{
    if (x < 0 || y < 0 || x >= fb.w || y >= fb.h) return;
    if (cov > 1.f) cov = 1.f;
    float* p = fb.at(x, y);
    float* o = ov.at(x, y);
    for (int c = 0; c < 3; ++c) {
        // mode 1 with coverage: d + (min(1, d + s) - d) * cov
        const float full = p[c] + s[c];
        const float kept = p[c] + (std::min(1.f, full) - p[c]) * cov;
        if (full > 1.f) o[c] += (full - 1.f) * cov;
        p[c] = kept;
    }
    p[3] = 1.f;
}

static inline bool finite4(float a, float b, float c, float d)
{
    return std::isfinite(a) && std::isfinite(b) && std::isfinite(c) && std::isfinite(d);
}

void plotDotAA(Framebuffer& fb, float x, float y, float radius,
               float r, float g, float b, int mode, float alpha)
{
    if (!std::isfinite(x) || !std::isfinite(y)) return;
    if (x < -64.f || y < -64.f || x > fb.w + 64.f || y > fb.h + 64.f) return;
    Framebuffer* ov = mode == 1 ? overflowFor(fb) : nullptr;
    const float src[3] = {r, g, b};
    auto put = [&](int px, int py, float cov) {
        if (ov) addCoverageHdr(fb, *ov, px, py, src, cov);
        else blendPixelCoverage(fb, px, py, r, g, b, mode, alpha, cov);
    };
    if (radius <= 0.75f) {
        // A one-pixel footprint [x-.5, x+.5] x [y-.5, y+.5] split over the
        // (up to) four pixels it overlaps: energy kept, motion sub-pixel.
        const float fx = x - 0.5f, fy = y - 0.5f;
        const int ix = (int)std::floor(fx), iy = (int)std::floor(fy);
        const float ax = fx - ix, ay = fy - iy;
        put(ix,     iy,     (1.f - ax) * (1.f - ay));
        put(ix + 1, iy,     ax * (1.f - ay));
        put(ix,     iy + 1, (1.f - ax) * ay);
        put(ix + 1, iy + 1, ax * ay);
        return;
    }
    const float reach = radius + 0.5f;
    const int xa = std::max(0, (int)std::floor(x - reach)), xb = std::min(fb.w - 1, (int)std::ceil(x + reach));
    const int ya = std::max(0, (int)std::floor(y - reach)), yb = std::min(fb.h - 1, (int)std::ceil(y + reach));
    for (int py = ya; py <= yb; ++py)
        for (int px = xa; px <= xb; ++px) {
            const float ex = px + 0.5f - x, ey = py + 0.5f - y;
            put(px, py, std::clamp(reach - std::sqrt(ex * ex + ey * ey), 0.f, 1.f));
        }
}

void drawSegmentAA(Framebuffer& fb, float x0, float y0, float x1, float y1, float halfWidth,
                   float r, float g, float b, int mode, float alpha, bool capStart, bool capEnd)
{
    if (!finite4(x0, y0, x1, y1)) return;
    auto cl = [](float v) { return std::clamp(v, -8192.f, 8192.f); };
    x0 = cl(x0); y0 = cl(y0); x1 = cl(x1); y1 = cl(y1);
    const float hw = std::max(0.5f, halfWidth);
    const float dx = x1 - x0, dy = y1 - y0;
    const float len2 = dx * dx + dy * dy;
    if (len2 < 1e-6f) {
        if (capStart || capEnd) plotDotAA(fb, x0, y0, hw, r, g, b, mode, alpha);
        return;
    }
    const float reach = hw + 0.5f;
    const float len = std::sqrt(len2), invLen = 1.f / len, invLen2 = 1.f / len2;
    const bool xMajor = std::fabs(dx) >= std::fabs(dy);
    // Minor-axis half-span that contains every pixel centre within reach of
    // the line in one major-axis step (reach / cos).
    const float span = reach * len / (xMajor ? std::fabs(dx) : std::fabs(dy));
    // How far past an end the major-axis scan must go: a full round cap
    // needs `reach`; an open end only the slant of the line's own edge.
    const float slant = reach * (xMajor ? std::fabs(dy) : std::fabs(dx)) * invLen;
    const float extStart = capStart ? reach : slant + 0.5f;
    const float extEnd = capEnd ? reach : slant + 0.5f;

    // Cheap in the common case: pixels off to the side are rejected on the
    // perpendicular distance, body pixels use it directly; only the pixels
    // past an end need the distance to the endpoint (a square root).
    auto coverage = [&](int px, int py) -> float {
        const float rx = px + 0.5f - x0, ry = py + 0.5f - y0;
        const float dperp = std::fabs(rx * dy - ry * dx) * invLen;
        if (dperp >= reach) return 0.f;
        const float t = (rx * dx + ry * dy) * invLen2;
        if (t < 0.f) {
            if (!capStart) return 0.f;                         // owned by the previous segment
            return std::clamp(reach - std::sqrt(rx * rx + ry * ry), 0.f, 1.f);
        }
        if (t > 1.f) {
            if (!capEnd) return 0.f;                           // owned by the next segment
            const float ex = rx - dx, ey = ry - dy;
            return std::clamp(reach - std::sqrt(ex * ex + ey * ey), 0.f, 1.f);
        }
        return std::min(1.f, reach - dperp);
    };

    // Thin lines: the one-pixel falloff sampled at pixel centres gains or
    // loses up to ~7% of its light depending on angle and sub-pixel offset
    // (worst at 45°), which makes moving diagonals shimmer. Each major-axis
    // step is rescaled so the INFINITE line through the segment would carry
    // exactly width/cos(angle) there. Because the factor comes from the
    // infinite line, not from this segment's own pixels, it is continuous
    // across polyline joints and caps. (Wide lines saturate in the middle
    // and barely alias; they are left alone.)
    const bool normalize = hw <= 1.51f;
    const CovBlendFn put = covBlendFor(mode);
    const float src[3] = {r, g, b};
    Framebuffer* ov = mode == 1 ? overflowFor(fb) : nullptr;   // HDR: keep what clips
    const float major = xMajor ? std::fabs(dx) : std::fabs(dy);
    const float expect = 2.f * hw * len / major;
    auto infCoverage = [&](float cx, float cy) {
        const float d = std::fabs((cy - y0) * dx - (cx - x0) * dy) * invLen;
        return std::clamp(reach - d, 0.f, 1.f);
    };

    if (xMajor) {
        const float xs = dx > 0 ? x0 - extStart : x1 - extEnd;
        const float xe = dx > 0 ? x1 + extEnd : x0 + extStart;
        const int xa = std::max(0, (int)std::floor(xs));
        const int xb = std::min(fb.w - 1, (int)std::ceil(xe));
        for (int px = xa; px <= xb; ++px) {
            float scale = 1.f;
            if (normalize) {
                const float yi = y0 + (px + 0.5f - x0) * dy / dx;          // the infinite line here
                float s = 0.f;
                for (int py = (int)std::floor(yi - span); py <= (int)std::ceil(yi + span); ++py)
                    s += infCoverage(px + 0.5f, py + 0.5f);
                if (s > 1e-4f) scale = expect / s;
            }
            const float t = std::clamp((px + 0.5f - x0) / dx, 0.f, 1.f);
            const float yc = y0 + t * dy - 0.5f;
            const int ya = std::max(0, (int)std::floor(yc - span));
            const int yb = std::min(fb.h - 1, (int)std::ceil(yc + span));
            for (int py = ya; py <= yb; ++py) {
                const float c = coverage(px, py);
                if (c <= 0.f) continue;
                if (ov) addCoverageHdr(fb, *ov, px, py, src, c * scale);
                else put(fb.at(px, py), src, alpha, c * scale);
            }
        }
    } else {
        const float ys = dy > 0 ? y0 - extStart : y1 - extEnd;
        const float ye = dy > 0 ? y1 + extEnd : y0 + extStart;
        const int ya = std::max(0, (int)std::floor(ys));
        const int yb = std::min(fb.h - 1, (int)std::ceil(ye));
        for (int py = ya; py <= yb; ++py) {
            float scale = 1.f;
            if (normalize) {
                const float xi = x0 + (py + 0.5f - y0) * dx / dy;
                float s = 0.f;
                for (int px = (int)std::floor(xi - span); px <= (int)std::ceil(xi + span); ++px)
                    s += infCoverage(px + 0.5f, py + 0.5f);
                if (s > 1e-4f) scale = expect / s;
            }
            const float t = std::clamp((py + 0.5f - y0) / dy, 0.f, 1.f);
            const float xc = x0 + t * dx - 0.5f;
            const int xa = std::max(0, (int)std::floor(xc - span));
            const int xb = std::min(fb.w - 1, (int)std::ceil(xc + span));
            for (int px = xa; px <= xb; ++px) {
                const float c = coverage(px, py);
                if (c <= 0.f) continue;
                if (ov) addCoverageHdr(fb, *ov, px, py, src, c * scale);
                else put(fb.at(px, py), src, alpha, c * scale);
            }
        }
    }
}

// Float positions beyond any screen are clamped before the Authentic cast
// (the integer rasterizer clamps to ±8192 anyway), so the cast is defined.
static inline int toPix(float v) { return (int)std::clamp(v, -1.0e6f, 1.0e6f); }

void drawLineQ(Framebuffer& fb, float x0, float y0, float x1, float y1,
               float r, float g, float b, int width, bool capStart, bool capEnd)
{
    const DrawQuality& q = drawQuality();
    if (!q.smooth) {
        if (!finite4(x0, y0, x1, y1)) return;
        drawLineMode(fb, toPix(x0), toPix(y0), toPix(x1), toPix(y1), r, g, b, width);
        return;
    }
    const LineMode& m = lineMode();
    const int w = std::clamp(width > 0 ? width : m.width, 1, 255);
    drawSegmentAA(fb, x0, y0, x1, y1, 0.5f * w * std::max(0.25f, q.widthScale),
                  r, g, b, m.blend, m.alpha, capStart, capEnd);
}

void putDotQ(Framebuffer& fb, float x, float y, float r, float g, float b)
{
    const DrawQuality& q = drawQuality();
    if (!q.smooth) {
        if (!std::isfinite(x) || !std::isfinite(y)) return;
        putLinePixel(fb, toPix(x), toPix(y), r, g, b);
        return;
    }
    const LineMode& m = lineMode();
    plotDotAA(fb, x, y, 0.5f * std::max(0.25f, q.widthScale), r, g, b, m.blend, m.alpha);
}

void drawLineMode(Framebuffer& fb, int x0, int y0, int x1, int y1, float r, float g, float b,
                  int widthOverride)
{
    const LineMode& m = lineMode();
    auto cl = [](int v) { return std::clamp(v, -8192, 8192); };
    x0 = cl(x0); y0 = cl(y0); x1 = cl(x1); y1 = cl(y1);
    const int width = std::clamp(widthOverride > 0 ? widthOverride : m.width, 1, 255);
    const int half = (width - 1) / 2;
    const bool steep = std::abs(y1 - y0) > std::abs(x1 - x0);
    for (int k = 0; k < width; ++k) {
        // Extra lines are offset along the minor axis, like vis_avs linedraw.h.
        const int off = k - half;
        int ax = x0, ay = y0, bx = x1, by = y1;
        if (steep) { ax += off; bx += off; } else { ay += off; by += off; }
        int dx = std::abs(bx - ax), sx = ax < bx ? 1 : -1;
        int dy = -std::abs(by - ay), sy = ay < by ? 1 : -1;
        int err = dx + dy;
        for (;;) {
            blendPixel(fb, ax, ay, r, g, b, m.blend, m.alpha);
            if (ax == bx && ay == by) break;
            int e2 = 2 * err;
            if (e2 >= dy) { err += dy; ax += sx; }
            if (e2 <= dx) { err += dx; ay += sy; }
        }
    }
}

void SetRenderModeEffect::render(Framebuffer& /*cur*/, const Framebuffer& /*prev*/,
                                 const VizFrame& /*a*/, const EffectContext& /*ctx*/)
{
    LineMode& m = lineMode();
    m.blend = std::clamp((int)blend, 0, 9);
    m.alpha = std::clamp(alpha, 0.f, 1.f);
    m.width = std::clamp((int)lineWidth, 1, 255);
}

// Blend `src` onto `dst` per Buffer Save's restore blend modes.
static void blendBuffers(Framebuffer& dst, const Framebuffer& src,
                         int mode, float adj)
{
    const int W = dst.w, H = dst.h;
    auto toByte = [](float v) { return (int)(std::clamp(v, 0.f, 1.f) * 255.f + 0.5f); };
    for (int y = 0; y < H; ++y) {
        const bool skipLine = (mode == 5) && (y & 1);
        for (int x = 0; x < W; ++x) {
            float* d = dst.at(x, y);
            const float* s = src.at(x, y);
            switch (mode) {
                default:
                case 0:  d[0] = s[0]; d[1] = s[1]; d[2] = s[2]; break;
                case 1:  for (int c = 0; c < 3; ++c) d[c] = (d[c] + s[c]) * 0.5f; break;
                case 2:  for (int c = 0; c < 3; ++c) d[c] = std::min(1.f, d[c] + s[c]); break;
                case 3:  if (!((x ^ y) & 1)) { d[0]=s[0]; d[1]=s[1]; d[2]=s[2]; } break;
                case 4:  for (int c = 0; c < 3; ++c) d[c] = std::max(0.f, d[c] - s[c]); break;
                case 5:  if (!skipLine) { d[0]=s[0]; d[1]=s[1]; d[2]=s[2]; } break;
                case 6:  for (int c = 0; c < 3; ++c)
                             d[c] = (toByte(d[c]) ^ toByte(s[c])) / 255.f;
                         break;
                case 7:  for (int c = 0; c < 3; ++c) d[c] = std::max(d[c], s[c]); break;
                case 8:  for (int c = 0; c < 3; ++c) d[c] = std::min(d[c], s[c]); break;
                case 9:  for (int c = 0; c < 3; ++c) d[c] = std::max(0.f, s[c] - d[c]); break;
                case 10: for (int c = 0; c < 3; ++c) d[c] = d[c] * s[c]; break;
                case 11: for (int c = 0; c < 3; ++c) d[c] += (s[c] - d[c]) * adj; break;
            }
        }
    }
}

void BufferSaveEffect::render(Framebuffer& cur, const Framebuffer& /*prev*/,
                              const VizFrame& /*a*/, const EffectContext& /*ctx*/)
{
    Framebuffer& buf = globalBuffer((int)buffer);

    bool doSave;
    switch ((int)action) {
        default:
        case 0: doSave = true; break;
        case 1: doSave = false; break;
        case 2: doSave = !_toggle; _toggle = !_toggle; break;   // save first
        case 3: doSave = _toggle;  _toggle = !_toggle; break;   // restore first
    }

    if (doSave) {
        if (buf.w != cur.w || buf.h != cur.h) buf.resize(cur.w, cur.h);
        buf.px = cur.px;
    } else {
        if (buf.w != cur.w || buf.h != cur.h) return;   // nothing saved yet
        blendBuffers(cur, buf, (int)blendMode, std::clamp(adjustable, 0.f, 1.f));
    }
}

void CustomBPMEffect::render(Framebuffer& /*cur*/, const Framebuffer& /*prev*/,
                             const VizFrame& a, const EffectContext& ctx)
{
    // The host renders effects against its own copy of the frame's VizFrame
    // (see EffectHost::renderFrame), so rewriting beat here only affects
    // effects later in this frame's stack — which is exactly the contract.
    VizFrame& fa = const_cast<VizFrame&>(a);

    if (fa.beat) _beatsSeen++;
    if (_beatsSeen <= (int)skipFirst) { fa.beat = false; return; }

    switch ((int)mode) {
        case 0: {                                  // fixed BPM, ignores input
            double interval = 60.0 / std::max(10.f, bpm);
            if (ctx.time - _lastFire >= interval) { _lastFire = ctx.time; fa.beat = true; }
            else fa.beat = false;
            break;
        }
        case 1:                                    // keep every (skip+1)th beat
            if (fa.beat) {
                if (++_skipCount > (int)skip) _skipCount = 0;
                else fa.beat = false;
            }
            break;
        case 2:                                    // invert
            fa.beat = !fa.beat;
            break;
        default: break;
    }
}

} // namespace viz
