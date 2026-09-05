//
// EffectList.cpp — see EffectList.h.
//
#include "EffectList.h"
#include "GpuFx.h"
#include "Profile.h"
#include "UtilEffects.h"
#include <algorithm>
#include <cmath>

namespace viz {

EffectListEffect::~EffectListEffect()
{
    gpu::invalidateResident(&_buf);
    gpu::invalidateResident(&_childPrev);
}

// dest = blend(dest, src) per AVS list blend mode id (see EffectList.h for
// the full table). `adjustable` is the 0..1 opacity for mode 10; `bufSlot` is
// the 0..7 global Buffer Save slot for mode 12 (-1 = none, treated as ignore).
static void applyBlend(Framebuffer& dest, const Framebuffer& src, int mode,
                       float adjustable = 0.5f, int bufSlot = -1, bool isOutput = false)
{
    if (mode == 0) return;                            // ignore
    // Every mode but Buffer (12) has a GPU twin working on both buffers'
    // memory in place; the loops below are the fallback and need the GPU
    // to have finished with both buffers first.
    if (mode != 12 && gpu::available() && gpu::blend(dest, src, mode, adjustable)) return;
    gpu::syncToCpuForRead(src);
    gpu::syncToCpu(dest);
    const int W = dest.w, H = dest.h;
    const int n = W * H;
    float* d = dest.px.data();
    const float* s = src.px.data();
    switch (mode) {
        case 2:                                       // 50/50
            for (int i = 0; i < n; ++i, d += 4, s += 4) {
                d[0] = (d[0] + s[0]) * 0.5f;
                d[1] = (d[1] + s[1]) * 0.5f;
                d[2] = (d[2] + s[2]) * 0.5f;
            }
            break;
        case 3:                                       // maximum
            for (int i = 0; i < n; ++i, d += 4, s += 4) {
                d[0] = std::max(d[0], s[0]);
                d[1] = std::max(d[1], s[1]);
                d[2] = std::max(d[2], s[2]);
            }
            break;
        case 4:                                       // additive
            for (int i = 0; i < n; ++i, d += 4, s += 4) {
                d[0] = std::min(1.f, d[0] + s[0]);
                d[1] = std::min(1.f, d[1] + s[1]);
                d[2] = std::min(1.f, d[2] + s[2]);
            }
            break;
        case 5:                                       // subtractive 1: dest - src
            for (int i = 0; i < n; ++i, d += 4, s += 4) {
                d[0] = std::max(0.f, d[0] - s[0]);
                d[1] = std::max(0.f, d[1] - s[1]);
                d[2] = std::max(0.f, d[2] - s[2]);
            }
            break;
        case 6:                                       // subtractive 2: src - dest
            for (int i = 0; i < n; ++i, d += 4, s += 4) {
                d[0] = std::max(0.f, s[0] - d[0]);
                d[1] = std::max(0.f, s[1] - d[1]);
                d[2] = std::max(0.f, s[2] - d[2]);
            }
            break;
        case 7:                                       // every other line: even rows = src
            for (int y = 0; y < H; ++y) {
                if ((y & 1) != 0) continue;
                float* row = d + (size_t)y * W * 4;
                const float* srow = s + (size_t)y * W * 4;
                std::copy(srow, srow + (size_t)W * 4, row);
            }
            break;
        case 8:                                       // every other pixel (checkerboard)
            for (int y = 0; y < H; ++y)
                for (int x = 0; x < W; ++x) {
                    if (((x + y) & 1) != 0) continue;
                    float* p = d + ((size_t)y * W + x) * 4;
                    const float* q = s + ((size_t)y * W + x) * 4;
                    p[0] = q[0]; p[1] = q[1]; p[2] = q[2];
                }
            break;
        case 9:                                       // xor (per-channel, 8-bit)
            for (int i = 0; i < n; ++i, d += 4, s += 4)
                for (int c = 0; c < 3; ++c) {
                    int a = (int)std::lround(std::clamp(d[c], 0.f, 1.f) * 255.f);
                    int b = (int)std::lround(std::clamp(s[c], 0.f, 1.f) * 255.f);
                    d[c] = (float)(a ^ b) / 255.f;
                }
            break;
        case 10: {                                     // adjustable: src*a + dest*(1-a)
            float a = std::clamp(adjustable, 0.f, 1.f);
            for (int i = 0; i < n; ++i, d += 4, s += 4) {
                d[0] = s[0] * a + d[0] * (1.f - a);
                d[1] = s[1] * a + d[1] * (1.f - a);
                d[2] = s[2] * a + d[2] * (1.f - a);
            }
            break;
        }
        case 11:                                       // multiply
            for (int i = 0; i < n; ++i, d += 4, s += 4) {
                d[0] *= s[0]; d[1] *= s[1]; d[2] *= s[2];
            }
            break;
        case 12: {                                     // buffer: route through a slot
            // Exact AVS Buffer-blend read/write direction isn't fully
            // reconstructed from source; bias toward never hiding content
            // that a plain Replace fallback would have shown.
            if (bufSlot >= 0 && isOutput) {             // output: publish + still show
                Framebuffer& buf = globalBuffer(bufSlot);
                if (buf.w != W || buf.h != H) buf.resize(W, H);
                buf.px = src.px;
            } else if (bufSlot >= 0) {                  // input: pull the slot in
                Framebuffer& buf = globalBuffer(bufSlot);
                if (buf.w == W && buf.h == H) { dest.px = buf.px; break; }
            }
            dest.px = src.px;                           // fallback: replace
            break;
        }
        case 13:                                       // minimum
            for (int i = 0; i < n; ++i, d += 4, s += 4) {
                d[0] = std::min(d[0], s[0]);
                d[1] = std::min(d[1], s[1]);
                d[2] = std::min(d[2], s[2]);
            }
            break;
        default:                                       // replace (1 and fallback)
            dest.px = src.px;
            break;
    }
    quantize8(dest.px);           // 8-bit truncation, as AVS (Framebuffer.h)
}

void EffectListEffect::render(Framebuffer& cur, const Framebuffer& /*prev*/,
                              const VizFrame& a, const EffectContext& ctx)
{
    const int W = cur.w, H = cur.h;
    if (W == 0 || H == 0 || _children.empty()) return;

    const int inMode = (int)inputBlend, outMode = (int)outputBlend;

    // Children see their working buffer's post-input state as "prev" — a
    // full-frame copy (and a GPU download when resident), taken only when a
    // child actually reads it.
    bool anyPrev = false;
    for (const auto& c : _children) if (c->enabled && c->usesPrev()) { anyPrev = true; break; }

    auto runChildren = [&](Framebuffer& target) {
        if (anyPrev) {
            gpu::syncToCpuForRead(target);
            _childPrev.resize(W, H);
            _childPrev.px = target.px;
        }
        for (auto& child : _children) {
            if (!child->enabled) continue;
            if (!child->isGpu()) prof::timed("(gpu sync)", [&] { gpu::syncToCpu(target); });
            prof::timed(child->name(), [&] { child->render(target, _childPrev, a, ctx); });
        }
    };

    // Replace in / Replace out means "copy the parent in, work, copy the
    // result back" — which is exactly rendering the children in place on
    // the parent. Every .avs preset's root list is this. It saves two
    // full-frame copies per frame and, when the last child ran on the GPU,
    // leaves the image on-texture for whatever follows (including present).
    // Clear-every-frame is moot under a Replace input, as in AVS itself.
    if (inMode == 1 && outMode == 1) {
        runChildren(cur);
        return;
    }

    _buf.resize(W, H);                       // no-op when the size is unchanged
    if (clearEveryFrame > 0.5f) {
        gpu::syncToCpu(_buf);                // the clear is a CPU write
        _buf.clear();
    }

    applyBlend(_buf, cur, inMode, inputAdjustable,
              (int)inputBlendBuffer, false);           // parent → list buffer

    runChildren(_buf);

    applyBlend(cur, _buf, outMode, outputAdjustable,
              (int)outputBlendBuffer, true);            // list buffer → parent
}

} // namespace viz
