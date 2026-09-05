//
// avs_selftest.cpp — load every .avs under a directory (recursively), check
// the JSON round-trip, render each over synthetic audio. Exit 0 when every
// file loaded.
//
//   avs_selftest <preset-dir> <out-dir>
//
// Knobs: AVS_FRAMES (default 240), AVS_NO_PNG=1, AVS_W/AVS_H (640x360),
// AVS_PROFILE=1 (per-effect cost ranking across the run).
//
#include "testsupport.h"
#include "AvsPreset.h"
#include "JsonPreset.h"
#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <map>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using namespace viz;

static std::string lower(std::string s)
{
    for (auto& c : s) c = (char)std::tolower((unsigned char)c);
    return s;
}

int main(int argc, const char** argv)
{
    setvbuf(stdout, nullptr, _IOLBF, 0);          // progress stays visible when redirected
    if (argc < 3) { printf("usage: avs_selftest <preset-dir> <out-dir>\n"); return 1; }
    const fs::path dir = argv[1], out = argv[2];

    std::vector<fs::path> files;
    std::error_code ec;
    for (auto it = fs::recursive_directory_iterator(dir, fs::directory_options::skip_permission_denied, ec);
         it != fs::recursive_directory_iterator(); it.increment(ec)) {
        if (ec) break;
        if (it->is_regular_file() && lower(it->path().extension().string()) == ".avs")
            files.push_back(it->path());
    }
    std::sort(files.begin(), files.end());
    if (files.empty()) { printf("no .avs files in %s\n", argv[1]); return 1; }

    const int simFrames = getenv("AVS_FRAMES") ? atoi(getenv("AVS_FRAMES")) : 240;
    const bool noPng = getenv("AVS_NO_PNG") != nullptr;
    const int simW = getenv("AVS_W") ? atoi(getenv("AVS_W")) : 640;
    const int simH = getenv("AVS_H") ? atoi(getenv("AVS_H")) : 360;
    const bool profile = getenv("AVS_PROFILE") != nullptr;
    struct Cost { double ms = 0; long frames = 0; };
    std::map<std::string, Cost> costs;
    double totalMs = 0; long totalFrames = 0;

    int okCount = 0, failCount = 0;
    for (const fs::path& file : files) {
        EffectHost host;
        host.resize(simW, simH);
        AvsLoadReport rep;
        bool ok = loadAvsPresetFile(host, file.string(), rep);
        printf("%s %s (stack: %zu)\n", ok ? "OK  " : "FAIL", rep.summary().c_str(),
               test::countEffects(host));
        if (!ok) { failCount++; continue; }

        {   // JSON round-trip: save -> load -> save must be identical
            std::string name = file.stem().string();
            std::string j1 = saveJsonPreset(host, name);
            EffectHost host2;
            std::string jerr;
            if (!loadJsonPreset(host2, j1, &jerr)) {
                printf("FAIL json load: %s\n", jerr.c_str());
                failCount++; continue;
            }
            std::string j2 = saveJsonPreset(host2, name);
            if (j1 != j2) {
                printf("FAIL json round-trip mismatch (%zu vs %zu bytes)\n", j1.size(), j2.size());
                failCount++; continue;
            }
        }

        VizFrame f;
        EffectContext ctx;
        double presetMs = 0; std::string presetTop; double presetTopMs = 0;
        for (int i = 0; i < simFrames; ++i) {
            test::synthAudio(f, i);
            ctx.frame = i; ctx.time = i / 60.0;
            host.renderFrame(f, ctx);
            if (profile) {
                double frameMs = 0;
                for (const auto& p : host.lastProfile()) {
                    auto& c = costs[p.first]; c.ms += p.second; c.frames++;
                    frameMs += p.second;
                    if (p.second > presetTopMs) { presetTopMs = p.second; presetTop = p.first; }
                }
                presetMs += frameMs; totalMs += frameMs; totalFrames++;
            }
        }
        if (profile)
            printf("     %.1f ms/frame, top: %s (%.1f ms)\n",
                   presetMs / std::max(1, simFrames), presetTop.c_str(), presetTopMs);
        if (!noPng) {
            std::string safe = fs::relative(file, dir, ec).replace_extension().string();
            for (auto& c : safe) if (c == '/' || c == '\\') c = '~'; else if (c == ' ') c = '_';
            test::writePng(host.currentSynced(), (out / ("avs_" + safe + ".png")).string());
        }
        okCount++;
    }
    printf("== %d loaded+rendered, %d failed ==\n", okCount, failCount);
    if (profile && totalFrames > 0) {
        std::vector<std::pair<std::string, Cost>> rank(costs.begin(), costs.end());
        std::sort(rank.begin(), rank.end(),
                  [](const auto& a, const auto& b) { return a.second.ms > b.second.ms; });
        printf("== profile @ %dx%d: %.2f ms/frame average over %ld frames ==\n",
               simW, simH, totalMs / totalFrames, totalFrames);
        printf("   %-28s %8s %8s %9s\n", "effect", "share", "ms/use", "uses");
        for (size_t i = 0; i < rank.size() && i < 25; ++i) {
            const auto& r = rank[i];
            printf("   %-28s %7.1f%% %8.2f %9ld\n", r.first.c_str(),
                   100.0 * r.second.ms / totalMs, r.second.ms / r.second.frames, r.second.frames);
        }
    }
    return failCount == 0 ? 0 : 2;
}
