//
// Superscope.cpp — see Superscope.h.
//
#include "Superscope.h"
#include "LineMode.h"
#include "SuperscopeExamples.h"
#include <algorithm>
#include <cmath>

namespace viz {

SuperscopeEffect::SuperscopeEffect() = default;

void SuperscopeEffect::setScripts(std::string init, std::string frame,
                                  std::string beat, std::string point)
{
    _sInit = std::move(init);
    _sFrame = std::move(frame);
    _sBeat = std::move(beat);
    _sPoint = std::move(point);
    example = -1.f;
    _scriptsDirty = true;
}

void SuperscopeEffect::rebuild(int w, int h)
{
    // Fresh VM per (re)build: scripts get a clean variable space, like AVS
    // loading an example. Rebinding is required afterwards.
    _vm = std::make_unique<eel::VM>();

    // Audio hooks. `_audio` is refreshed every render() before scripts run.
    auto sample = [this](const float* buf, int len, double band, double width) {
        int lo = (int)std::floor(std::clamp(band - width * 0.5, 0.0, 1.0) * (len - 1));
        int hi = (int)std::ceil(std::clamp(band + width * 0.5, 0.0, 1.0) * (len - 1));
        if (hi < lo) std::swap(lo, hi);
        double s = 0; int cnt = 0;
        for (int k = lo; k <= hi; ++k) { s += buf[k]; ++cnt; }
        return cnt ? s / cnt : 0.0;
    };
    _vm->getosc = [this, sample](double band, double width, double ch) {
        if (!_audio) return 0.0;
        int c = ((int)ch == 2) ? 1 : 0;
        double v = sample(_audio->waveform[c], kWaveformSamples, band, width);
        if ((int)ch == 0 && _audio->numWaveformChannels > 1) {
            v = (v + sample(_audio->waveform[1], kWaveformSamples, band, width)) * 0.5;
        }
        return v;
    };
    _vm->getspec = [this, sample](double band, double width, double ch) {
        if (!_audio) return 0.0;
        int c = ((int)ch == 2) ? 1 : 0;
        double v = sample(_audio->spectrum[c], kSpectrumBins, band, width);
        if ((int)ch == 0 && _audio->numSpectrumChannels > 1) {
            v = (v + sample(_audio->spectrum[1], kSpectrumBins, band, width)) * 0.5;
        }
        return v;
    };
    _vm->gettime = [this](double start) { return _time - start; };

    // Bind the AVS variable contract.
    _n = _vm->var("n"); _b = _vm->var("b"); _i = _vm->var("i"); _v = _vm->var("v");
    _x = _vm->var("x"); _y = _vm->var("y"); _w = _vm->var("w"); _h = _vm->var("h");
    _red = _vm->var("red"); _green = _vm->var("green"); _blue = _vm->var("blue");
    _linesize = _vm->var("linesize"); _skip = _vm->var("skip");
    _drawmode = _vm->var("drawmode");

    *_n = 100; *_red = 1; *_green = 1; *_blue = 1;
    *_drawmode = 1; *_linesize = 1;
    *_w = w; *_h = h;

    _pInit  = eel::compile(*_vm, _sInit);
    _pFrame = eel::compile(*_vm, _sFrame);
    _pBeat  = eel::compile(*_vm, _sBeat);
    _pPoint = eel::compile(*_vm, _sPoint);

    _pInit.run();
    _scriptsDirty = false;
}

// Dots and lines go through the global line mode (LineMode.h): AVS's default
// is replace; a Set Render Mode component earlier in the stack changes it.
static void putReplace(Framebuffer& fb, int x, int y, float r, float g, float b)
{
    putLinePixel(fb, x, y, r, g, b);
}

static void drawLine(Framebuffer& fb, float x0, float y0, float x1, float y1,
                     float r, float g, float b, int width)
{
    drawLineMode(fb, (int)x0, (int)y0, (int)x1, (int)y1, r, g, b, width);
}

void SuperscopeEffect::render(Framebuffer& cur, const Framebuffer& /*prev*/,
                              const VizFrame& a, const EffectContext& ctx)
{
    const int W = cur.w, H = cur.h;
    if (W == 0 || H == 0) return;

    // (Re)compile when the selected example changed or custom scripts were set.
    int ex = (int)example;
    if (example >= 0.f && ex != _compiledExample) {
        ex = std::clamp(ex, 0, kNumSuperscopeExamples - 1);
        const auto& e = kSuperscopeExamples[ex];
        _sInit = e.init; _sFrame = e.frame; _sBeat = e.beat; _sPoint = e.point;
        _compiledExample = ex;
        _scriptsDirty = true;
    }
    if (example < 0.f && _scriptsDirty) _compiledExample = -1;
    if (_scriptsDirty || !_vm) rebuild(W, H);

    _audio = &a;
    _time = ctx.time;
    *_w = W; *_h = H;
    *_b = a.beat ? 1.0 : 0.0;
    *_vm->var("bass") = a.bass;
    *_vm->var("mid") = a.mid;
    *_vm->var("treb") = a.treble;
    *_vm->var("bpm") = a.bpm;
    *_vm->var("beatphase") = a.beatPhase;

    if (a.beat) _pBeat.run();
    _pFrame.run();

    int n = (int)*_n;
    n = std::clamp(n, 1, 4096);

    const int chanSel = (int)channel;      // 0 center, 1 left, 2 right
    const bool useSpec = (int)source == 1;

    float px = 0, py = 0;
    bool havePrev = false;
    for (int idx = 0; idx < n; ++idx) {
        double i01 = (n == 1) ? 0.0 : (double)idx / (n - 1);

        // v: audio value at this scope position
        int si = (int)(i01 * ((useSpec ? kSpectrumBins : kWaveformSamples) - 1));
        double v;
        if (useSpec) {
            v = (chanSel == 2) ? a.spectrum[1][si]
              : (chanSel == 1) ? a.spectrum[0][si]
              : (a.spectrum[0][si] + a.spectrum[1][si]) * 0.5;
        } else {
            v = (chanSel == 2) ? a.waveform[1][si]
              : (chanSel == 1) ? a.waveform[0][si]
              : (a.waveform[0][si] + a.waveform[1][si]) * 0.5;
        }

        *_i = i01;
        *_v = v;
        *_skip = 0;
        _pPoint.run();

        // AVS coords: x,y in -1..1, y=+1 is the bottom. Our framebuffer is
        // y-up, so flip. With an aspect mode set, both axes share one scale
        // so scripted shapes stay round on wide windows.
        const float SX = ctx.coordSX > 0 ? ctx.coordSX : (W - 1) * 0.5f;
        const float SY = ctx.coordSY > 0 ? ctx.coordSY : (H - 1) * 0.5f;
        float fx = (float)((W - 1) * 0.5 + std::clamp(*_x, -4.0, 4.0) * SX);
        float fy = (float)((H - 1) * 0.5 - std::clamp(*_y, -4.0, 4.0) * SY);

        float r = (float)std::clamp(*_red, 0.0, 1.0) * gain;
        float g = (float)std::clamp(*_green, 0.0, 1.0) * gain;
        float b = (float)std::clamp(*_blue, 0.0, 1.0) * gain;

        if (*_skip <= 0.0) {
            if (*_drawmode > 0.0 && havePrev)   // AVS passes the scope's own linesize
                drawLine(cur, px, py, fx, fy, r, g, b,
                         std::clamp((int)(*_linesize + 0.5), 1, 255));
            else
                putReplace(cur, (int)fx, (int)fy, r, g, b);
        }
        px = fx; py = fy;
        havePrev = *_skip <= 0.0;
    }
}

} // namespace viz
