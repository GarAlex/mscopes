//
// effects_selftest.cpp — render every built-in preset, or every registered
// effect solo (gallery), over synthetic audio to PNGs. Exit 0 on success.
//
//   effects_selftest [outdir]           — built-in presets → preset_N.png
//   effects_selftest gallery [outdir]   — every effect → fx_<key>.png
//
#include "testsupport.h"
#include "BuiltinEffects.h"
#include "Presets.h"
#include <cstdio>
#include <cstring>

using namespace viz;

// Every registered effect over a standard base (motionless fade + dim scope
// as source material) — eyeball any new port there.
static int runGallery(const std::string& outDir)
{
    const int W = 640, H = 360, FRAMES = 180;
    int failures = 0;
    for (const auto& [key, factory] : effectRegistry()) {
        EffectHost host;
        host.resize(W, H);
        {
            auto fade = effectRegistry().at("feedback_warp")();
            fade->setParam("zoom", 1.0f); fade->setParam("zoom_bass", 0.f);
            fade->setParam("spin", 0.f);  fade->setParam("spin_treble", 0.f);
            fade->setParam("beat_kick", 0.f); fade->setParam("decay", 0.93f);
            host.add(std::move(fade));
        }
        {
            auto scope = effectRegistry().at("scope")();
            scope->setParam("gain", 0.55f);
            host.add(std::move(scope));
        }
        host.add(factory());                       // the effect under test

        VizFrame f;
        EffectContext ctx;
        for (int i = 0; i < FRAMES; ++i) {
            test::synthAudio(f, i);
            ctx.frame = i; ctx.time = i / 60.0;
            host.renderFrame(f, ctx);
        }
        std::string path = outDir + "/fx_" + key + ".png";
        bool ok = test::writePng(host.currentSynced(), path);
        printf(">> fx \"%s\": -> %s (ok=%d)\n", key.c_str(), path.c_str(), ok);
        if (!ok) failures++;
    }
    printf(">> gallery: %zu effects rendered, %d failures\n", effectRegistry().size(), failures);
    return failures ? 2 : 0;
}

int main(int argc, const char** argv)
{
    if (argc > 1 && std::strcmp(argv[1], "gallery") == 0)
        return runGallery(argc > 2 ? argv[2] : "build");

    const std::string outDir = argc > 1 ? argv[1] : "build";
    const int W = 640, H = 360, FRAMES = 240;

    const auto& presets = builtinPresets();
    int failures = 0;
    for (size_t pi = 0; pi < presets.size(); ++pi) {
        EffectHost host;
        host.resize(W, H);
        applyPreset(host, presets[pi]);

        VizFrame f;
        EffectContext ctx;
        for (int i = 0; i < FRAMES; ++i) {
            test::synthAudio(f, i);
            ctx.frame = i; ctx.time = i / 60.0;
            host.renderFrame(f, ctx);
        }
        std::string path = outDir + "/preset_" + std::to_string(pi) + ".png";
        bool ok = test::writePng(host.currentSynced(), path);
        printf(">> preset %zu \"%s\": %zu effects, %d frames -> %s (ok=%d)\n",
               pi, presets[pi].name.c_str(), host.count(), FRAMES, path.c_str(), ok);
        if (!ok) failures++;
    }
    return failures ? 2 : 0;
}
