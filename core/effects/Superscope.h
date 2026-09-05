//
// Superscope.h — the scripted Superscope effect, our take on AVS SuperScope
// (e_superscope.cpp, BSD-3), running on our own EEL VM.
//
// Scripts: init (once), frame (per frame), beat (on beat), point (n times per
// frame). Script variables per the AVS contract: n, b, i, v, x, y, w, h,
// red, green, blue, linesize, skip, drawmode. getosc/getspec/gettime hooks
// are bound to the current audio frame.
//
// Ships with the 14 classic example scripts (SuperscopeExamples.h) selectable
// via the `example` param; custom scripts can be set programmatically (the
// script editor UI and .avs loading build on that).
//
#pragma once
#include "Effect.h"
#include "EelVM.h"
#include <memory>
#include <string>

namespace viz {

class SuperscopeEffect : public Effect {
public:
    SuperscopeEffect();
    const char* name() const override { return "Superscope"; }
    void render(Framebuffer& cur, const Framebuffer& prev,
                const VizFrame& a, const EffectContext& ctx) override;

    float example = 0.f;    // bundled example index (truncated); -1 = custom
    float source  = 0.f;    // 0 = waveform, 1 = spectrum
    float channel = 0.f;    // 0 = center (avg), 1 = left, 2 = right
    float gain    = 1.0f;   // brightness multiplier

    std::vector<Param> params() override {
        return {{"example", 0.f, 13.f, &example},
                {"source", 0.f, 1.f, &source},
                {"channel", 0.f, 2.f, &channel},
                {"gain", 0.2f, 2.f, &gain}};
    }

    // Set custom scripts (switches example to custom mode; recompiles lazily).
    void setScripts(std::string init, std::string frame,
                    std::string beat, std::string point);


    // Script access for editors: returns {init, frame, beat, point}.
    void getScripts(std::string& i, std::string& f, std::string& b, std::string& p) const {
        i = _sInit; f = _sFrame; b = _sBeat; p = _sPoint;
    }

private:
    void rebuild(int w, int h);            // fresh VM + compile + run init

    std::unique_ptr<eel::VM> _vm;
    eel::Program _pInit, _pFrame, _pBeat, _pPoint;
    std::string _sInit, _sFrame, _sBeat, _sPoint;

    int _compiledExample = -999;           // which example is compiled (-1 custom)
    bool _scriptsDirty = false;

    // bound script variables (valid while _vm lives)
    double *_n = nullptr, *_b = nullptr, *_i = nullptr, *_v = nullptr;
    double *_x = nullptr, *_y = nullptr, *_w = nullptr, *_h = nullptr;
    double *_red = nullptr, *_green = nullptr, *_blue = nullptr;
    double *_linesize = nullptr, *_skip = nullptr, *_drawmode = nullptr;

    const VizFrame* _audio = nullptr;      // for getosc/getspec hooks
    double _time = 0;
};

} // namespace viz
