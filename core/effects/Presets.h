//
// Presets.h — named effect stacks. A preset is a list of (effect type, params,
// enabled); applying one rebuilds the host's stack. This is the AVS "preset"
// concept: the same effect pool composing into very different looks.
//
// In-code for now; the same structures serialize naturally to JSON later.
//
#pragma once
#include "Effect.h"
#include "EffectHost.h"
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace viz {

// type name → factory
using EffectFactory = std::function<std::unique_ptr<Effect>()>;
const std::map<std::string, EffectFactory>& effectRegistry();

struct PresetEffect {
    std::string type;                                   // registry key
    std::vector<std::pair<std::string, float>> params;  // key → value
    bool enabled = true;
};

struct Preset {
    std::string name;
    std::vector<PresetEffect> effects;
    bool modern = false;           // uses this project's original effects
    bool inheritCanvas = false;    // start on the previous image, not black
};

// The built-in preset library.
const std::vector<Preset>& builtinPresets();

// True for effects original to this project (GPU/modern set) rather than
// AVS ports — the app groups its browser by this.
bool isModernEffectKey(const std::string& key);

// Rebuild `host`'s stack from `preset`. Unknown effect types are skipped.
void applyPreset(EffectHost& host, const Preset& preset);

// Reset all process-global state presets can see (shared reg00-99, gmegabuf,
// the 8 Buffer Save framebuffers, Multi Delay queues). Call when loading a
// preset so results don't depend on what ran before.
void resetSharedState();

} // namespace viz
