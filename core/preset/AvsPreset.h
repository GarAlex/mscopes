//
// AvsPreset.h — loader for classic Winamp AVS `.avs` preset files.
//
// Parses the legacy binary format ("Nullsoft AVS Preset 0.1/0.2\x1a" +
// effect-list body) and instantiates our effects into an EffectHost. Format
// knowledge derived from grandchild/vis_avs (BSD-3).
//
// v1 scope:
//  - Scripted effects (Superscope, Dynamic Movement) load their FULL scripts —
//    the soul of a preset comes through.
//  - A set of common effects decode their key config fields; others found in
//    the registry are instantiated with defaults (reported as "partial").
//  - Nested effect lists are flattened into the flat stack (blend modes of
//    sublists are not yet honored).
//  - Unknown/unsupported components are skipped and reported.
//
#pragma once
#include "EffectHost.h"
#include <cstdint>
#include <string>
#include <vector>

namespace viz {

struct AvsLoadReport {
    std::string presetName;
    std::vector<std::string> loaded;    // instantiated with config
    std::vector<std::string> partial;   // instantiated with defaults only
    std::vector<std::string> skipped;   // not supported yet
    std::string error;                  // non-empty on fatal parse failure

    bool ok() const { return error.empty(); }
    std::string summary() const;
};

// Parse `data` and rebuild `host`'s stack from it. Returns false on fatal
// error (bad magic / truncated); partial success still returns true.
//
// forceInherit: skip this preset's own clear-vs-inherit auto-detection and
// always inherit the existing canvas without clearing. For sequential
// "album" playback (numbered preset series meant to flow into one another,
// e.g. visbot's VE/"Visual Episode" packs) — real AVS never clears between
// preset switches, and later tracks are often pure modifiers authored to
// warp whatever the previous track left on screen. Use this for every track
// after the first when playing such a sequence; the first track should
// still use its own classification (false) for a clean start.
bool loadAvsPreset(EffectHost& host, const uint8_t* data, size_t len,
                   AvsLoadReport& report, bool forceInherit = false);

// Convenience: read the file at `path`.
bool loadAvsPresetFile(EffectHost& host, const std::string& path,
                       AvsLoadReport& report, bool forceInherit = false);

} // namespace viz
