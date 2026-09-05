//
// EffectList.h — nested effect list (group) with AVS blend semantics, our take
// on AVS Effect List (e_effectlist.cpp, BSD-3).
//
// A list owns a persistent buffer. Each frame: optionally clear it, blend the
// parent frame IN (input mode), render children onto it, then blend the result
// back OUT into the parent (output mode). This is what makes layered presets
// composite correctly instead of flattening into one additive soup.
//
#pragma once
#include "Effect.h"
#include <memory>
#include <vector>

namespace viz {

// AVS list blend mode ids, per vis_avs e_effectlist.h blend_modes() (all
// implemented): 0 Ignore, 1 Replace, 2 50/50, 3 Maximum, 4 Additive,
// 5 Subtractive 1 (dest-src), 6 Subtractive 2 (src-dest), 7 Every Other Line,
// 8 Every Other Pixel, 9 XOR, 10 Adjustable (opacity), 11 Multiply,
// 12 Buffer (read/write one of the 8 global Buffer Save slots), 13 Minimum.
class EffectListEffect : public Effect {
public:
    ~EffectListEffect() override;    // invalidates GPU residency of _buf
    const char* name() const override { return "Effect List"; }
    void render(Framebuffer& cur, const Framebuffer& prev,
                const VizFrame& a, const EffectContext& ctx) override;

    void addChild(std::unique_ptr<Effect> e) { _children.push_back(std::move(e)); }
    size_t childCount() const { return _children.size(); }
    Effect* childAt(size_t i) { return i < _children.size() ? _children[i].get() : nullptr; }

    float inputBlend  = 1.f;   // how the parent frame enters the list buffer
    float outputBlend = 1.f;   // how the list result returns to the parent
    float clearEveryFrame = 0.f;
    float inputAdjustable  = 0.5f;  // mode 10 opacity, 0..1
    float outputAdjustable = 0.5f;
    float inputBlendBuffer  = -1.f; // mode 12 global-buffer slot, 0..7 (-1 = none)
    float outputBlendBuffer = -1.f;

    std::vector<Param> params() override {
        return {{"input_blend", 0.f, 13.f, &inputBlend},
                {"output_blend", 0.f, 13.f, &outputBlend},
                {"clear_every_frame", 0.f, 1.f, &clearEveryFrame},
                {"input_adjustable", 0.f, 1.f, &inputAdjustable},
                {"output_adjustable", 0.f, 1.f, &outputAdjustable},
                {"input_blend_buffer", -1.f, 7.f, &inputBlendBuffer},
                {"output_blend_buffer", -1.f, 7.f, &outputBlendBuffer}};
    }

private:
    std::vector<std::unique_ptr<Effect>> _children;
    Framebuffer _buf;        // the list's persistent working buffer
    Framebuffer _childPrev;  // snapshot children may use as "previous frame"
};

} // namespace viz
