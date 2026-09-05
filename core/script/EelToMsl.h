//
// EelToMsl.h — transpile a compiled EEL program into a Metal compute kernel.
//
// This is what makes per-PIXEL scripted effects possible: the interpreter
// holds 60fps per point/grid-cell, but per pixel only the GPU can. The
// CPU-side init/frame/beat scripts keep running on the VM as usual; the pixel
// script is walked as an AST and emitted as MSL with EEL semantics preserved
// (eager if/band/bor, div-by-zero → 0, integer %, auto-created vars).
//
// Variables the host provides per pixel (x, y, d, r, red, green, blue, w, h)
// are declared by the kernel wrapper. Every other variable the script touches
// becomes a float uniform, snapshotted from the VM after the frame script runs
// — so frame-script state (t, bass, reg00, …) flows into the pixel script.
//
// Unsupported in pixel scripts (they stay CPU-side concepts): getosc, getspec,
// gettime, megabuf, gmegabuf, assign(). Transpilation fails with an error.
//
#pragma once
#include "EelVM.h"
#include <string>
#include <utility>
#include <vector>

namespace viz { namespace eel {

struct MslResult {
    std::string source;            // full kernel source; empty on failure
    std::vector<double*> uniforms; // VM slots to upload, in u[kUniformBase+i] order
    std::string error;
};

// First uniform index available to script variables. Reserved below:
// u[0] = per-frame RNG seed, u[1]/u[2] = x/y pixel scales (aspect modes).
constexpr int kMslUniformBase = 4;

MslResult transpileToMsl(const Program& prog,
                         const std::vector<std::pair<double*, const char*>>& pixelVars);

}} // namespace viz::eel
