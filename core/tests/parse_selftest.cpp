//
// parse_selftest.cpp — the file parsers against damaged input.
//
// MScopes opens .avs presets, its own .json presets and .cube looks that
// come from the internet, in an app that can't be sandboxed (the audio tap).
// This feeds each parser thousands of deterministic corruptions of real
// files (flipped bytes, huge sizes, truncation, duplicated chunks) and, for
// presets that still load, renders two small frames. Pass = nothing crashes
// or hangs. Under the sanitizers (cmake -DMSCOPES_SANITIZE=ON) it also
// catches out-of-bounds reads and undefined behaviour.
//
//   parse_selftest <fixture-dir> [rounds]
//
#include "testsupport.h"
#include "AvsPreset.h"
#include "JsonPreset.h"
#include "LineMode.h"
#include "Picture.h"
#include "Presets.h"
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <random>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using namespace viz;

static std::mt19937 rng(20261111);

static size_t pick(size_t n) { return n ? std::uniform_int_distribution<size_t>(0, n - 1)(rng) : 0; }

// One random corruption of a byte string.
static void mutate(std::vector<uint8_t>& d, bool text)
{
    if (d.empty()) { d.push_back((uint8_t)pick(256)); return; }
    static const char kText[] = "{}[]\":,0123456789-+eE.aZ \n\\/";
    switch (pick(6)) {
    case 0: d[pick(d.size())] = (uint8_t)pick(256); break;                       // any byte
    case 1: d[pick(d.size())] = text ? (uint8_t)kText[pick(sizeof kText - 1)]     // a structural one
                                     : (uint8_t)(pick(2) ? 0xff : 0x00); break;
    case 2: {                                                                     // a huge or odd 32-bit value
        if (d.size() < 4) break;
        const uint32_t vals[] = {0xffffffffu, 0x7fffffffu, 0x80000000u, 0x10000u, 0u, 1u};
        uint32_t v = vals[pick(6)];
        size_t at = pick(d.size() - 3);
        std::memcpy(&d[at], &v, 4);
        break;
    }
    case 3: d.resize(pick(d.size())); break;                                      // truncate
    case 4: {                                                                     // duplicate a chunk
        size_t a = pick(d.size()), n = std::min<size_t>(pick(64) + 1, d.size() - a);
        std::vector<uint8_t> chunk(d.begin() + (long)a, d.begin() + (long)(a + n));
        d.insert(d.begin() + (long)pick(d.size()), chunk.begin(), chunk.end());
        break;
    }
    default: {                                                                    // delete a chunk
        size_t a = pick(d.size()), n = std::min<size_t>(pick(64) + 1, d.size() - a);
        d.erase(d.begin() + (long)a, d.begin() + (long)(a + n));
        break;
    }
    }
}

static void renderTwo(EffectHost& host)
{
    VizFrame f;
    EffectContext ctx;
    for (int i = 0; i < 2; ++i) {
        test::synthAudio(f, i);
        ctx.frame = (uint64_t)i;
        ctx.time = i / 60.0;
        host.renderFrame(f, ctx);
    }
    host.currentSynced();
}

int main(int argc, const char** argv)
{
    setvbuf(stdout, nullptr, _IOLBF, 0);
    if (argc < 2) { printf("usage: parse_selftest <fixture-dir> [rounds]\n"); return 2; }
    const int rounds = argc > 2 ? std::max(1, atoi(argv[2])) : 150;
    drawQuality() = DrawQuality{};
    int loaded = 0, total = 0;

    // --- .avs: corruptions of every fixture -------------------------------
    std::vector<fs::path> files;
    for (const auto& e : fs::directory_iterator(argv[1]))
        if (e.path().extension() == ".avs") files.push_back(e.path());
    std::sort(files.begin(), files.end());
    for (const auto& p : files) {
        std::ifstream in(p, std::ios::binary);
        const std::vector<uint8_t> orig((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        for (int r = 0; r < rounds; ++r) {
            std::vector<uint8_t> d = orig;
            for (int k = 0, n = 1 + (int)pick(4); k < n; ++k) mutate(d, false);
            EffectHost host;
            host.resize(64, 36);
            AvsLoadReport rep;
            ++total;
            if (loadAvsPreset(host, d.data(), d.size(), rep)) { ++loaded; renderTwo(host); }
        }
    }
    printf("avs:  %d corrupted presets, %d still loaded and rendered\n", total, loaded);

    // --- .json: corruptions of every built-in preset, saved as JSON --------
    int jt = 0, jl = 0;
    for (const auto& pr : builtinPresets()) {
        EffectHost src;
        src.resize(64, 36);
        applyPreset(src, pr);
        const std::string json = saveJsonPreset(src, pr.name);
        for (int r = 0; r < rounds / 3 + 1; ++r) {
            std::vector<uint8_t> d(json.begin(), json.end());
            for (int k = 0, n = 1 + (int)pick(4); k < n; ++k) mutate(d, true);
            EffectHost host;
            host.resize(64, 36);
            std::string err;
            ++jt;
            if (loadJsonPreset(host, std::string(d.begin(), d.end()), &err)) { ++jl; renderTwo(host); }
        }
    }
    printf("json: %d corrupted presets, %d still loaded and rendered\n", jt, jl);

    // --- .cube: corruptions of a small valid look ---------------------------
    std::string cube = "TITLE \"test\"\nDOMAIN_MIN 0 0 0\nDOMAIN_MAX 1 1 1\nLUT_3D_SIZE 4\n";
    for (int b = 0; b < 4; ++b)
        for (int g = 0; g < 4; ++g)
            for (int r = 0; r < 4; ++r) {
                char line[64];
                snprintf(line, sizeof line, "%.4f %.4f %.4f\n", r / 3.0, g / 3.0, b / 3.0);
                cube += line;
            }
    int ct = 0, cl = 0;
    for (int r = 0; r < rounds * 4; ++r) {
        std::vector<uint8_t> d(cube.begin(), cube.end());
        for (int k = 0, n = 1 + (int)pick(4); k < n; ++k) mutate(d, true);
        LookTable t;
        std::string err;
        ++ct;
        if (parseCube(std::string(d.begin(), d.end()), t, &err)) {
            ++cl;
            float in[3] = {0.5f, 0.25f, 1.f}, out[3];
            sampleLook(t, in, out);
        }
    }
    printf("cube: %d corrupted looks, %d still parsed\n", ct, cl);

    printf("parse_selftest: ALL PASS (nothing crashed)\n");
    return 0;
}
