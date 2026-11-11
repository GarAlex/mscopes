//
// SharedState.h — the state AVS presets share between their effects: the
// script registers reg00..reg99, gmegabuf, the eight Buffer Save buffers,
// the six Multi Delay buffers, and the line mode set by Set Render Mode.
//
// Effects reach it through process-wide storage, as AVS's did (compiled
// scripts hold pointers straight to the registers). Each EffectHost owns its
// own: the storage holds the ACTIVE host's state, and a host becomes active
// when it renders or a preset is loaded into it (EffectHost::activate()),
// parking the previous host's state. With one host, as in the app, nothing
// ever moves. A crossfade's outgoing preset keeps its state the same way:
// loading the next preset resets the storage, so the host swaps the outgoing
// copy in around the outgoing frames (before, a preset that placed its
// scopes from registers collapsed while it faded out).
//
#pragma once
#include "DelayEffects.h"
#include "Framebuffer.h"
#include "LineMode.h"
#include <unordered_map>
#include <vector>

namespace viz {

struct SharedState {
    double regs[100] = {};
    std::unordered_map<int, std::vector<double>> gmegabuf;
    Framebuffer buffers[8];
    FrameQueue delays[6];
    LineMode lineMode;
};

// Exchange the process-wide shared state with `s` (contents, so pointers
// that compiled scripts hold to the registers stay valid). Cheap: the large
// parts are moved, not copied.
void swapSharedState(SharedState& s);

// Reset the process-wide shared state (the active host's). Preset loaders
// call it so results don't depend on what ran before.
void resetSharedState();

} // namespace viz
