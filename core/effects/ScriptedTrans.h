//
// ScriptedTrans.h — scripted transform effects on the EEL VM, our takes on
// AVS Trans / Color Modifier (e_colormodifier.cpp), Trans / Dynamic Shift
// (e_dynamicshift.cpp) and Trans / Dynamic Distance Modifier
// (e_dynamicdistancemodifier.cpp), all BSD-3.
//
// Like Superscope/DynamicMovement these are script *hosts*: behavior lives in
// the preset's EEL scripts, the C++ just owns the variable contract and the
// pixel loop.
//
#pragma once
#include "Effect.h"
#include "EelVM.h"
#include <memory>
#include <string>

namespace viz {

// Base plumbing shared by the scripted transforms: script storage, recompile
// tracking, and the init/frame/beat execution ritual.
class ScriptedEffectBase : public Effect {
public:
    void setScripts(std::string init, std::string frame,
                    std::string beat, std::string point = std::string());
    void getScripts(std::string& i, std::string& f, std::string& b, std::string& p) const {
        i = _sInit; f = _sFrame; b = _sBeat; p = _sPoint;
    }

protected:
    // Recompile if dirty (calls bindVars() on the fresh VM). Call before
    // touching bound host vars each frame.
    void ensureCompiled();
    // Run init-once / beat / frame scripts (sets b first).
    void runFrameScripts(const VizFrame& a);
    // Convenience: both, for effects with no pre-frame host vars to set.
    void prepareFrame(const VizFrame& a) { ensureCompiled(); runFrameScripts(a); }
    virtual void bindVars(eel::VM& vm) = 0;

    std::unique_ptr<eel::VM> _vm;
    eel::Program _pInit, _pFrame, _pBeat, _pPoint;
    std::string _sInit, _sFrame, _sBeat, _sPoint;
    bool _scriptsDirty = true;
    bool _inited = false;
    double* _b = nullptr;          // beat flag, bound for every subclass
    const VizFrame* _audioFrame = nullptr;  // refreshed by runFrameScripts
    double _time = 0;              // set by render() for gettime()
};

// Trans / Color Modifier: the `level` (point) script maps each channel value —
// run for the 256 levels it builds a per-channel LUT applied to every pixel.
// Variables: red, green, blue (0..1), beat.
class ColorModifierEffect : public ScriptedEffectBase {
public:
    ColorModifierEffect();
    const char* name() const override { return "Color Modifier"; }
    void render(Framebuffer& cur, const Framebuffer& prev,
                const VizFrame& a, const EffectContext& ctx) override;

    float recompute = 1.f;         // >0.5: rebuild the LUT every frame

    std::vector<Param> params() override {
        return {{"recompute", 0.f, 1.f, &recompute}};
    }

private:
    void bindVars(eel::VM& vm) override;
    double *_red = nullptr, *_green = nullptr, *_blue = nullptr;
    float _lut[3][256];
    bool _lutValid = false;
};

// Trans / Dynamic Shift: frame/beat scripts drive an x/y framebuffer shift in
// pixels; uncovered area goes black. Variables: x, y, w, h, b, alpha.
class DynamicShiftEffect : public ScriptedEffectBase {
public:
    DynamicShiftEffect();
    const char* name() const override { return "Dynamic Shift"; }
    bool isGpu() const override;   // scripts on the CPU VM, the shift on Metal
    void render(Framebuffer& cur, const Framebuffer& prev,
                const VizFrame& a, const EffectContext& ctx) override;

    float blend    = 0.f;          // >0.5: mix shifted with original by alpha
    float bilinear = 1.f;          // >0.5: subpixel sampling

    std::vector<Param> params() override {
        return {{"blend", 0.f, 1.f, &blend},
                {"bilinear", 0.f, 1.f, &bilinear}};
    }

private:
    void bindVars(eel::VM& vm) override;
    double *_x = nullptr, *_y = nullptr, *_alpha = nullptr;
    double *_w = nullptr, *_h = nullptr;
    Framebuffer _src;
};

// Trans / Dynamic Distance Modifier: the point script maps normalized
// distance-from-center d (0..1); pixels are pulled along their ray to the
// remapped distance. Variables: d, b.
class DynamicDistanceModifierEffect : public ScriptedEffectBase {
public:
    DynamicDistanceModifierEffect();
    const char* name() const override { return "Dynamic Distance Modifier"; }
    bool isGpu() const override;   // scripts + table on the CPU VM, the remap on Metal
    void render(Framebuffer& cur, const Framebuffer& prev,
                const VizFrame& a, const EffectContext& ctx) override;

    float blend    = 0.f;          // >0.5: 50/50 with original
    float bilinear = 1.f;          // >0.5: subpixel sampling

    std::vector<Param> params() override {
        return {{"blend", 0.f, 1.f, &blend},
                {"bilinear", 0.f, 1.f, &bilinear}};
    }

private:
    void bindVars(eel::VM& vm) override;
    double* _d = nullptr;
    std::vector<float> _table;     // per-distance-step scale factor
    Framebuffer _src;
};

// Render / Triangle ("Render: Triangle" APE): the point script emits n
// triangles per frame (vertices in -1..1, Gouraud vertex colors, optional
// z-buffer). Variables: n, i, skip, x1..y3, z1, red1..blue3, zbuf, zbclear,
// w, h, b.
class TriangleEffect : public ScriptedEffectBase {
public:
    TriangleEffect();
    const char* name() const override { return "Triangle"; }
    void render(Framebuffer& cur, const Framebuffer& prev,
                const VizFrame& a, const EffectContext& ctx) override;

private:
    void bindVars(eel::VM& vm) override;
    double *_n = nullptr, *_i = nullptr, *_skip = nullptr;
    double *_x1 = nullptr, *_y1 = nullptr, *_z1 = nullptr;
    double *_x2 = nullptr, *_y2 = nullptr, *_x3 = nullptr, *_y3 = nullptr;
    double *_r1 = nullptr, *_g1 = nullptr, *_b1 = nullptr;
    double *_r2 = nullptr, *_g2 = nullptr, *_b2 = nullptr;
    double *_r3 = nullptr, *_g3 = nullptr, *_b3 = nullptr;
    double *_zbuf = nullptr, *_zbclear = nullptr;
    double *_w = nullptr, *_h = nullptr;
    std::vector<float> _depth;
};

// Directory where preset-relative resources (Texer bitmaps) are looked up.
// The .avs loader points this at the preset file's folder.
void setPresetResourceDir(const std::string& dir);
const std::string& presetResourceDir();

// Render / Texer II ("Acko.net: Texer II" APE): the point script places n
// textured sprites per frame. Textures load from a .bmp next to the preset
// (24/32-bit uncompressed); a soft radial glow is the fallback. Variables:
// n, i, x, y, w, h, iw, ih, sizex, sizey, red, green, blue, skip, v, b.
class TexerIIEffect : public ScriptedEffectBase {
public:
    TexerIIEffect();
    const char* name() const override { return "Texer II"; }
    void render(Framebuffer& cur, const Framebuffer& prev,
                const VizFrame& a, const EffectContext& ctx) override;

    float colorize  = 1.f;   // >0.5: modulate the texture by red/green/blue
    float wrap      = 0.f;   // >0.5: sprites wrap around screen edges
    float blendMode = 1.f;   // 0 replace, 1 additive, 2 maximum, 3 50/50

    // Use a .bmp from the preset resource dir ("" = built-in glow).
    void setImage(std::string name) { _imageName = std::move(name); _texLoaded = false; }

    std::vector<Param> params() override {
        return {{"colorize", 0.f, 1.f, &colorize},
                {"wrap", 0.f, 1.f, &wrap},
                {"blend_mode", 0.f, 3.f, &blendMode}};
    }

private:
    void bindVars(eel::VM& vm) override;
    void loadTexture();
    std::string _imageName;
    bool _texLoaded = false;
    int _texW = 0, _texH = 0;
    std::vector<float> _tex;   // rgb triplets
    double *_n = nullptr, *_i = nullptr, *_x = nullptr, *_y = nullptr;
    double *_w = nullptr, *_h = nullptr, *_iw = nullptr, *_ih = nullptr;
    double *_sizex = nullptr, *_sizey = nullptr;
    double *_red = nullptr, *_green = nullptr, *_blue = nullptr;
    double *_skip = nullptr, *_v = nullptr;
};

// Misc / Global Variables ("Jheriko: Global" APE): a pure computation block —
// init/frame/beat scripts that read/write the shared reg00-99 and gmegabuf so
// other scripted effects can pick the values up. Draws nothing itself.
// Variables: w, h, b. (The original's file load/save is not implemented.)
class GlobalVariablesEffect : public ScriptedEffectBase {
public:
    GlobalVariablesEffect();
    const char* name() const override { return "Global Variables"; }
    void render(Framebuffer& cur, const Framebuffer& prev,
                const VizFrame& a, const EffectContext& ctx) override;

private:
    void bindVars(eel::VM& vm) override;
    double *_w = nullptr, *_h = nullptr;
};

} // namespace viz
