//
// JsonPreset.h — our native preset format: the effect stack as JSON.
//
// Serializes registry key, enabled flag, every introspectable param, the four
// EEL scripts of scripted effects, and nested effect lists. Round-trips
// losslessly (save → load → save yields identical text), which is also how
// the selftest verifies it.
//
#pragma once
#include "EffectHost.h"
#include <string>

namespace viz {

// Serialize the host's current stack to pretty-printed JSON.
std::string saveJsonPreset(EffectHost& host, const std::string& name);

// Replace the host's stack with the one described by `json`.
// On failure returns false, sets *err if given, and leaves the host empty.
bool loadJsonPreset(EffectHost& host, const std::string& json,
                    std::string* err = nullptr);

// File convenience wrappers.
bool saveJsonPresetFile(EffectHost& host, const std::string& name,
                        const std::string& path);
bool loadJsonPresetFile(EffectHost& host, const std::string& path,
                        std::string* err = nullptr);

} // namespace viz
