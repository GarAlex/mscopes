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
