//
// Effect.h — the effect ABI. Our clean-room analogue of the AVS effect model
// (audio + previous frame in, pixels out), designed to compose in a stack and
// to map onto a GPU backend later.
//
// AVS was: render(char visdata[2][2][576], int is_beat, int* fb, int* fbout, w, h).
// Ours takes the shared VizFrame (already-analyzed audio) + the previous frame
// (for feedback) and mutates the current framebuffer.
//
#pragma once
#include "VizFrame.h"
#include "Framebuffer.h"
#include <string>
#include <vector>

namespace viz {

struct EffectContext {
    double   time  = 0.0;      // seconds
    double   dt    = 1.0/60.0; // seconds since last frame
    uint64_t frame = 0;

    // Script-space (-1..1) → pixel scale, set by the host app. 0 = classic
    // AVS stretch (x spans width, y spans height — shapes distort on wide
    // windows). Equal non-zero values keep script shapes round: fill
    // (max(W,H)/2, crops the short axis) or fit (min(W,H)/2).
    float coordSX = 0.f, coordSY = 0.f;
};

// A named, bounded, live-bound float parameter. `value` points into the owning
// effect, so writing through it takes effect on the next rendered frame. This
// is the seam presets, the SwiftUI editor, and (later) scripting all share.
struct Param {
    const char* key;
    float minV, maxV;
    float* value;
};

class Effect {
public:
    virtual ~Effect() {}
    virtual const char* name() const = 0;

    // Mutate `cur`. `prev` holds the previous frame's final output (for feedback).
    virtual void render(Framebuffer& cur, const Framebuffer& prev,
                        const VizFrame& audio, const EffectContext& ctx) = 0;

    // Introspectable parameters (empty by default).
    virtual std::vector<Param> params() { return {}; }

    // True for effects that render via the GPU backend. The host uses this to
    // keep the image resident on-texture across consecutive GPU effects and
    // only sync back to CPU memory when a CPU effect needs the pixels.
    virtual bool isGpu() const { return false; }

    // True for effects that read `prev`. Hosts only snapshot the previous
    // frame (a full-frame copy, plus a GPU download when the image is
    // resident) when some enabled effect in the stack actually wants it.
    virtual bool usesPrev() const { return false; }

    // Convenience: set a param by key (no-op if unknown; clamped to range).
    void setParam(const std::string& key, float v) {
        for (auto& p : params())
            if (key == p.key) { *p.value = std::min(p.maxV, std::max(p.minV, v)); return; }
    }

    bool enabled = true;
};

} // namespace viz
