//
// DynamicMovement.h — the scripted Dynamic Movement effect, our take on AVS
// Dynamic Movement (e_dynamicmovement.cpp, BSD-3), running on our EEL VM.
//
// The point script runs once per grid VERTEX (not per pixel). For each vertex
// it receives its position as x,y (-1..1) and d (0..1 of the half-diagonal),
// r (atan2 + pi/2), mutates them, and the result becomes that vertex's source
// coordinate. Per pixel, source coords are bilinearly interpolated across the
// grid cell and the previous content is resampled — AVS's exact scheme.
//
// Variables: x, y, d, r, b, alpha (per-vertex blend amount), w, h.
//
#pragma once
#include "Effect.h"
#include "EelVM.h"
#include <memory>
#include <string>

namespace viz {

class DynamicMovementEffect : public Effect {
public:
    DynamicMovementEffect();
    const char* name() const override { return "Dynamic Movement"; }
    // The point script always runs on the CPU VM (per grid vertex); the
    // per-pixel resample runs on Metal when available.
    bool isGpu() const override;
    void render(Framebuffer& cur, const Framebuffer& prev,
                const VizFrame& a, const EffectContext& ctx) override;

    float example = 0.f;   // bundled example index; -1 = custom scripts
    float gridW   = 2.f;   // grid vertex counts (set by example on load)
    float gridH   = 2.f;
    float rect    = 0.f;   // >0.5: script output read from x/y, else d/r
    float wrap    = 1.f;   // >0.5: wrap source coords, else clamp
    float blend   = 0.f;   // >0.5: blend result with original by per-vertex alpha

    std::vector<Param> params() override {
        return {{"example", 0.f, 7.f, &example},
                {"grid_w", 2.f, 64.f, &gridW},
                {"grid_h", 2.f, 64.f, &gridH},
                {"rect", 0.f, 1.f, &rect},
                {"wrap", 0.f, 1.f, &wrap},
                {"blend", 0.f, 1.f, &blend}};
    }

    // Custom scripts (switches to custom mode; caller also sets grid/rect/wrap).
    void setScripts(std::string init, std::string frame,
                    std::string beat, std::string point);


    // Script access for editors: returns {init, frame, beat, point}.
    void getScripts(std::string& i, std::string& f, std::string& b, std::string& p) const {
        i = _sInit; f = _sFrame; b = _sBeat; p = _sPoint;
    }

private:
    void rebuild();

    std::unique_ptr<eel::VM> _vm;
    eel::Program _pInit, _pFrame, _pBeat, _pPoint;
    std::string _sInit, _sFrame, _sBeat, _sPoint;
    int _compiledExample = -999;
    bool _scriptsDirty = false;
    bool _inited = false;

    double *_x = nullptr, *_y = nullptr, *_d = nullptr, *_r = nullptr;
    double *_b = nullptr, *_alpha = nullptr, *_w = nullptr, *_h = nullptr;

    std::vector<float> _grid;   // per-vertex: srcX, srcY, alpha (3 floats)
    Framebuffer _src;           // snapshot of the incoming frame
};

} // namespace viz
