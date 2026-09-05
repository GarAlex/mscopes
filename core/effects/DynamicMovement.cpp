//
// DynamicMovement.cpp — see DynamicMovement.h.
//
#include "DynamicMovement.h"
#include "DynamicMovementExamples.h"
#include "GpuFx.h"
#include <algorithm>
#include <cmath>

namespace viz {

DynamicMovementEffect::DynamicMovementEffect() = default;

bool DynamicMovementEffect::isGpu() const { return gpu::available(); }

void DynamicMovementEffect::setScripts(std::string init, std::string frame,
                                       std::string beat, std::string point)
{
    _sInit = std::move(init);
    _sFrame = std::move(frame);
    _sBeat = std::move(beat);
    _sPoint = std::move(point);
    example = -1.f;
    _scriptsDirty = true;
}

void DynamicMovementEffect::rebuild()
{
    _vm = std::make_unique<eel::VM>();
    _x = _vm->var("x"); _y = _vm->var("y");
    _d = _vm->var("d"); _r = _vm->var("r");
    _b = _vm->var("b"); _alpha = _vm->var("alpha");
    _w = _vm->var("w"); _h = _vm->var("h");
    *_alpha = 0.5;                       // AVS default

    _pInit  = eel::compile(*_vm, _sInit);
    _pFrame = eel::compile(*_vm, _sFrame);
    _pBeat  = eel::compile(*_vm, _sBeat);
    _pPoint = eel::compile(*_vm, _sPoint);
    _scriptsDirty = false;
    _inited = false;
}

void DynamicMovementEffect::render(Framebuffer& cur, const Framebuffer& /*prev*/,
                                   const VizFrame& a, const EffectContext& ctx)
{
    const int W = cur.w, H = cur.h;
    if (W == 0 || H == 0) return;

    // (Re)load example / recompile custom scripts.
    int ex = (int)example;
    if (example >= 0.f && ex != _compiledExample) {
        ex = std::clamp(ex, 0, kNumDynamicMovementExamples - 1);
        const auto& e = kDynamicMovementExamples[ex];
        _sInit = e.init; _sFrame = e.frame; _sBeat = e.beat; _sPoint = e.point;
        gridW = (float)e.gridW; gridH = (float)e.gridH;
        rect = e.rectangular ? 1.f : 0.f;
        wrap = e.wrap ? 1.f : 0.f;
        _compiledExample = ex;
        _scriptsDirty = true;
    }
    if (_scriptsDirty || !_vm) rebuild();

    *_w = W; *_h = H;
    *_b = a.beat ? 1.0 : 0.0;
    *_vm->var("bass") = a.bass;
    *_vm->var("mid") = a.mid;
    *_vm->var("treb") = a.treble;
    *_vm->var("bpm") = a.bpm;
    *_vm->var("beatphase") = a.beatPhase;
    if (!_inited) { _pInit.run(); _inited = true; }
    if (a.beat) _pBeat.run();
    _pFrame.run();

    const int GX = std::clamp((int)gridW, 2, 64);
    const int GY = std::clamp((int)gridH, 2, 64);
    const bool usePolar = rect <= 0.5f;
    const bool doWrap = wrap > 0.5f;
    const bool doBlend = blend > 0.5f;

    const double cx = W * 0.5, cy = H * 0.5;
    const double maxD = std::sqrt((double)(W * W + H * H)) * 0.5;

    // Aspect mode: with equal per-axis scales, script-space circles stay
    // round. 0 → classic AVS stretch (exact legacy math below).
    const double SX = ctx.coordSX > 0 ? ctx.coordSX : W * 0.5;
    const double SY = ctx.coordSY > 0 ? ctx.coordSY : H * 0.5;
    const bool aspectFix = ctx.coordSX > 0 || ctx.coordSY > 0;
    const double maxDA = std::sqrt(2.0);        // half-diagonal in unit space

    // --- per-vertex: run the point script, record source coords + alpha ---
    _grid.resize((size_t)GX * GY * 3);
    for (int gy = 0; gy < GY; ++gy) {
        for (int gx = 0; gx < GX; ++gx) {
            double pxd = (double)gx / (GX - 1) * W - cx;   // centered pixels
            double pyd = (double)gy / (GY - 1) * H - cy;

            if (aspectFix) {
                double ux = pxd / SX, uy = pyd / SY;
                *_x = ux;
                *_y = uy;
                *_d = std::sqrt(ux * ux + uy * uy) / maxDA;
                *_r = std::atan2(uy, ux) + M_PI * 0.5;
            } else {
                *_x = pxd * (2.0 / W);
                *_y = pyd * (2.0 / H);
                *_d = std::sqrt(pxd * pxd + pyd * pyd) / maxD;
                *_r = std::atan2(pyd, pxd) + M_PI * 0.5;
            }

            _pPoint.run();

            double sx, sy;
            if (usePolar) {
                double rr = *_r - M_PI * 0.5;
                if (aspectFix) {
                    double dd = *_d * maxDA;
                    sx = cx + std::cos(rr) * dd * SX;
                    sy = cy + std::sin(rr) * dd * SY;
                } else {
                    double dd = *_d * maxD;
                    sx = cx + std::cos(rr) * dd;
                    sy = cy + std::sin(rr) * dd;
                }
            } else if (aspectFix) {
                sx = cx + *_x * SX;
                sy = cy + *_y * SY;
            } else {
                sx = (*_x + 1.0) * 0.5 * W;
                sy = (*_y + 1.0) * 0.5 * H;
            }
            if (!doWrap) {
                sx = std::clamp(sx, 0.0, (double)(W - 1));
                sy = std::clamp(sy, 0.0, (double)(H - 1));
            }
            float* v = &_grid[((size_t)gy * GX + gx) * 3];
            v[0] = (float)sx;
            v[1] = (float)sy;
            v[2] = (float)std::clamp(*_alpha, 0.0, 1.0);
        }
    }

    // --- per-pixel: bilinear-interp source coords across the grid, resample ---
    if (gpu::available()) { gpu::gridWarp(cur, _grid.data(), GX, GY, doWrap, doBlend); return; }

    _src.resize(W, H);
    _src.px = cur.px;                     // snapshot of incoming frame

    float sample[4];
    for (int y = 0; y < H; ++y) {
        double fy = (double)y / (H - 1) * (GY - 1);
        int gy0 = std::min((int)fy, GY - 2);
        float ty = (float)(fy - gy0);
        const float* row0 = &_grid[(size_t)gy0 * GX * 3];
        const float* row1 = &_grid[(size_t)(gy0 + 1) * GX * 3];

        for (int x = 0; x < W; ++x) {
            double fx = (double)x / (W - 1) * (GX - 1);
            int gx0 = std::min((int)fx, GX - 2);
            float tx = (float)(fx - gx0);

            const float* v00 = &row0[(size_t)gx0 * 3];
            const float* v10 = &row0[(size_t)(gx0 + 1) * 3];
            const float* v01 = &row1[(size_t)gx0 * 3];
            const float* v11 = &row1[(size_t)(gx0 + 1) * 3];

            float sx = (v00[0] + (v10[0] - v00[0]) * tx) * (1 - ty)
                     + (v01[0] + (v11[0] - v01[0]) * tx) * ty;
            float sy = (v00[1] + (v10[1] - v00[1]) * tx) * (1 - ty)
                     + (v01[1] + (v11[1] - v01[1]) * tx) * ty;

            if (doWrap) {
                sx = std::fmod(sx, (float)W); if (sx < 0) sx += W;
                sy = std::fmod(sy, (float)H); if (sy < 0) sy += H;
            }
            _src.sample(sx, sy, sample);

            float* dpx = cur.at(x, y);
            if (doBlend) {
                float al = (v00[2] + (v10[2] - v00[2]) * tx) * (1 - ty)
                         + (v01[2] + (v11[2] - v01[2]) * tx) * ty;
                dpx[0] += (sample[0] - dpx[0]) * al;
                dpx[1] += (sample[1] - dpx[1]) * al;
                dpx[2] += (sample[2] - dpx[2]) * al;
            } else {
                dpx[0] = sample[0];
                dpx[1] = sample[1];
                dpx[2] = sample[2];
            }
        }
    }
    quantize8(cur.px);            // 8-bit truncation, as AVS (Framebuffer.h)
}

} // namespace viz
