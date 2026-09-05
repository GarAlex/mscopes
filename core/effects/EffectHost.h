//
// EffectHost.h — owns the framebuffers and runs an effect stack each frame.
//
// Model: effects run in order on `_cur`. Feedback effects read `_prev` (the
// previous frame's final output). After the stack runs, `_cur` is snapshotted
// into `_prev` for next frame. This mirrors AVS's framebuffer/fbout swap while
// staying simple.
//
#pragma once
#include "Effect.h"
#include "EelVM.h"
#include "GpuFx.h"
#include "Profile.h"
#include <memory>
#include <string>
#include <vector>

namespace viz {

// A reg → effect-param link: each frame the param is set to the reg's value
// (clamped to the param's range). This is the Milkdrop "per-frame equations
// steer the pipeline" idea: a Global Variables script computes reg00-99, and
// bound params follow. Persisted in JSON presets.
struct ParamBinding {
    size_t effectIndex;
    std::string paramKey;
    int reg;                       // 0..99
};

class EffectHost {
public:
    ~EffectHost() {
        gpu::invalidateResident(&_cur);
        gpu::invalidateResident(&_prev);
        gpu::invalidateResident(&_outCur);
        gpu::invalidateResident(&_outPrev);
    }
    void resize(int w, int h) {
        if (w == _cur.w && h == _cur.h) return;
        _cur.resize(w, h);
        _prev.resize(w, h);
    }

    void add(std::unique_ptr<Effect> e) { _effects.push_back(std::move(e)); }

    // Clears the stack — and by default the canvas too: feedback effects
    // recycle whatever is on screen indefinitely, so a normal preset must
    // start from black to be deterministic. Fragment/modifier presets
    // (transform-only stacks) pass clearCanvas=false to inherit the image —
    // that inheritance is their entire purpose. (During a crossfade the
    // outgoing copy keeps the old image either way, so transitions blend.)
    void clearEffects(bool clearCanvas = true) {
        _effects.clear();
        _bindings.clear();
        inheritCanvas = false;
        if (clearCanvas) this->clearCanvas();
    }

    void clearCanvas() {
        gpu::invalidateResident(&_cur);
        gpu::invalidateResident(&_prev);
        if (_cur.w) _cur.clear();
        if (_prev.w) _prev.clear();
    }

    // Set by preset loaders; saved into JSON presets ("canvas": "inherit").
    bool inheritCanvas = false;

    // Param bindings (reg-driven params). Caller keeps indices consistent
    // with the current stack; bindings are cleared with it.
    std::vector<ParamBinding>& paramBindings() { return _bindings; }
    void setBinding(size_t effectIndex, const std::string& key, int reg) {
        for (auto& b : _bindings)
            if (b.effectIndex == effectIndex && b.paramKey == key) {
                if (reg < 0) { b = _bindings.back(); _bindings.pop_back(); }
                else b.reg = reg;
                return;
            }
        if (reg >= 0) _bindings.push_back({effectIndex, key, reg});
    }
    int bindingFor(size_t effectIndex, const std::string& key) const {
        for (const auto& b : _bindings)
            if (b.effectIndex == effectIndex && b.paramKey == key) return b.reg;
        return -1;
    }

    // Milkdrop-style preset blending: snapshot the current stack (and its
    // framebuffers) as "outgoing", then let the caller load the new stack as
    // usual. renderFrame crossfades the two for `seconds`.
    void beginCrossfade(double seconds) {
        if (_effects.empty() || seconds <= 0) return;
        _outgoing = std::move(_effects);
        _effects.clear();
        _bindings.clear();
        gpu::syncToCpuForRead(_cur);          // the image may still be on-texture
        _outCur.resize(_cur.w, _cur.h);
        _outPrev.resize(_cur.w, _cur.h);
        _outCur.px = _cur.px;
        _outPrev.px = _prev.px;
        _fadeT = 0;
        _fadeDur = seconds;
    }
    size_t count() const { return _effects.size(); }
    Effect* at(size_t i) { return i < _effects.size() ? _effects[i].get() : nullptr; }

    void removeAt(size_t i) {
        if (i < _effects.size()) _effects.erase(_effects.begin() + i);
    }
    // Move the effect at `from` to position `to` (both clamped).
    void move(size_t from, size_t to) {
        if (from >= _effects.size()) return;
        if (to >= _effects.size()) to = _effects.size() - 1;
        auto e = std::move(_effects[from]);
        _effects.erase(_effects.begin() + from);
        _effects.insert(_effects.begin() + to, std::move(e));
    }

    // Run the stack for one frame; returns the current framebuffer.
    // Effects see a per-frame COPY of `audio` so beat-rewriting effects
    // (Custom BPM) can mutate it for the rest of this frame's stack without
    // touching the caller's data.
    const Framebuffer& renderFrame(const VizFrame& audio, const EffectContext& ctx) {
        _audio = audio;
        prof::beginFrame();

        // reg-driven params (clamped to each param's declared range)
        for (const auto& b : _bindings)
            if (Effect* e = at(b.effectIndex))
                for (auto& p : e->params())
                    if (b.paramKey == p.key) {
                        float v = (float)*eel::VM::globalRegSlot(b.reg);
                        *p.value = std::min(p.maxV, std::max(p.minV, v));
                        break;
                    }

        for (auto& e : _effects) {
            if (!e->enabled) continue;
            // GPU effects keep the image on-texture between themselves; sync
            // the pixels back only when a CPU effect needs them.
            if (!e->isGpu()) prof::timed("(gpu sync)", [&] { gpu::syncToCpu(_cur); });
            prof::timed(e->name(), [&] { e->render(_cur, _prev, _audio, ctx); });
        }
        // The previous-frame snapshot only exists for effects that read it.
        // Otherwise the frame can stay on-texture straight into present().
        if (anyUsesPrev(_effects)) {
            prof::timed("(gpu sync)", [&] { gpu::syncToCpuForRead(_cur); });
            prof::timed("(prev snapshot)", [&] { _prev.px = _cur.px; });
        }

        // crossfade the outgoing stack during a preset transition
        if (!_outgoing.empty()) {
            // Only one framebuffer is ever GPU-resident: bring _cur home
            // before the outgoing stack claims the texture (the blend below
            // writes _cur on the CPU anyway).
            gpu::syncToCpu(_cur);
            for (auto& e : _outgoing) {
                if (!e->enabled) continue;
                if (!e->isGpu()) prof::timed("(gpu sync)", [&] { gpu::syncToCpu(_outCur); });
                prof::timed(e->name(), [&] { e->render(_outCur, _outPrev, _audio, ctx); });
            }
            gpu::syncToCpuForRead(_outCur);
            if (anyUsesPrev(_outgoing)) _outPrev.px = _outCur.px;

            _fadeT += ctx.dt / std::max(0.05, _fadeDur);
            float t = (float)std::min(1.0, _fadeT);
            t = t * t * (3.f - 2.f * t);              // smooth crossfade
            if (_outCur.px.size() == _cur.px.size()) {
                float* c = _cur.px.data();
                const float* o = _outCur.px.data();
                for (size_t i = 0; i < _cur.px.size(); ++i)
                    c[i] = o[i] + (c[i] - o[i]) * t;
            }
            if (_fadeT >= 1.0) {
                _outgoing.clear();           // effect dtors invalidate their own buffers
                gpu::invalidateResident(&_outCur);
                gpu::invalidateResident(&_outPrev);
                _outCur = Framebuffer();
                _outPrev = Framebuffer();
            }
        }
        return _cur;
    }

    // The current frame's framebuffer. Its pixel mirror may be stale while the
    // image is GPU-resident: use current() for size/identity (and for
    // gpu::present(), which reads the texture), currentSynced() to READ pixels.
    const Framebuffer& current() const { return _cur; }
    const Framebuffer& currentSynced() const { gpu::syncToCpuForRead(_cur); return _cur; }

    // Per-effect self time (ms) of the last renderFrame, costliest first.
    // Nested list children are reported individually by their own names.
    std::vector<std::pair<std::string, double>> lastProfile() const { return prof::summary(); }

private:
    static bool anyUsesPrev(const std::vector<std::unique_ptr<Effect>>& fx) {
        for (const auto& e : fx) if (e->enabled && e->usesPrev()) return true;
        return false;
    }

    Framebuffer _cur, _prev;
    VizFrame _audio;          // per-frame working copy (mutable beat)
    std::vector<std::unique_ptr<Effect>> _effects;
    std::vector<ParamBinding> _bindings;

    // preset-transition state
    std::vector<std::unique_ptr<Effect>> _outgoing;
    Framebuffer _outCur, _outPrev;
    double _fadeT = 0, _fadeDur = 0;
};

} // namespace viz
