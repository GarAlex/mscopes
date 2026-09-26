//
// Picture.h — display-stage picture settings: colour looks, grain, vignette,
// glow, scanlines, sharp scaling, dither.
//
// These act only on what is drawn to the screen, in the final pass that
// presents the finished frame (gpu::present / gpu::renderPicture). Nothing
// here is ever written back into the frame a preset keeps: most presets feed
// their last frame into the next one, and a tint applied there would
// compound every frame. Classic presets therefore render exactly as
// authored underneath any look.
//
// Portable C++: the look tables and the .cube reader live here so they can
// be tested on any platform; the pass itself is in the GPU backend.
//
#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace viz {

// Edge length of the baked look tables (33 is the common .cube size).
static constexpr int kLookSize = 33;

// A 3D colour lookup table over display values 0..1: size^3 RGB triplets,
// red varying fastest, then green, then blue (the .cube order).
struct LookTable {
    int size = 0;
    std::vector<float> rgb;
    uint64_t version = 0;      // unique per content; the GPU re-uploads on change

    bool empty() const { return size < 2 || rgb.size() < (size_t)size * size * size * 3; }
    void clear() { size = 0; rgb.clear(); version = 0; }
};

// Built-in looks. Index 0 is "Off".
const std::vector<std::string>& lookNames();

// Bake built-in look `index` into `out` (kLookSize^3). Index 0 empties it.
// Returns false (and leaves `out` alone) for an unknown index.
bool buildLook(int index, LookTable& out);

// Read a .cube file's text (3D or 1D table, any DOMAIN / INPUT_RANGE) and
// bake it into `out` at kLookSize^3. On failure returns false with a short
// reason in *err and leaves `out` alone.
bool parseCube(const std::string& text, LookTable& out, std::string* err);
bool loadCubeFile(const std::string& path, LookTable& out, std::string* err);

// Trilinear lookup on the CPU (tests, and anything without a GPU).
void sampleLook(const LookTable& t, const float in[3], float out[3]);

// Everything the picture pass needs for one frame.
struct PictureSettings {
    const LookTable* look = nullptr;   // nullptr or empty = no look
    float lookStrength = 1.f;          // 0..1, mix between original and look
    float grain = 0.f;                 // 0..1
    float vignette = 0.f;              // 0..1
    float glow = 0.f;                  // 0..1, soft multi-scale bloom
    float scanlines = 0.f;             // 0..1
    bool  beatReactive = false;        // grain and glow breathe with the music
    bool  sharpScaling = true;         // de-ringed bicubic instead of bilinear upscale
    bool  dither = true;               // last step: removes banding in 8-bit output

    // Per frame.
    float bass = 0.f;                  // 0..1
    float beat = 0.f;                  // 1 on a detected beat, decaying toward 0
    uint32_t frame = 0;                // animates grain and dither
    float pointScale = 1.f;            // output pixels per point (grain, scanline size)

    bool hasLook() const { return look && !look->empty() && lookStrength > 0.f; }
};

} // namespace viz
