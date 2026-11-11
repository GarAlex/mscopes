//
// golden_selftest.cpp — renders every test fixture (presets/tests/*.avs) and
// every built-in preset over the synthetic test audio and compares the last
// frame with stored references, so a change that alters what presets look
// like is caught by the tests instead of by eye.
//
// Two checks per preset, references in core/tests/golden/:
//   <backend>.txt               a 16x9 thumbnail (cell means, 0..255 per
//                               channel), compared with a tolerance. Works
//                               across compilers and platforms, catches a
//                               preset washing out, going black or changing
//                               colour. Simulations (fluid, reaction-
//                               diffusion) amplify rounding differences, so
//                               for them only the overall colour and a loose
//                               layout are compared.
//   <backend>-<toolchain>.hash  an exact hash of the full 8-bit frame. Checked
//                               only when it was made with the same backend,
//                               platform and compiler, in a plain Release
//                               build (float results differ between
//                               toolchains and optimisation levels), so it
//                               guards "classic presets render byte for byte
//                               as before" on the machine that made it.
// <backend> is "metal" when the GPU backend is running and "cpu" otherwise
// (MSCOPES_NO_GPU=1, and every non-Apple platform).
//
//   golden_selftest <fixture-dir> <golden-dir> <out-dir>            check
//   golden_selftest <fixture-dir> <golden-dir> <out-dir> --update   rewrite references
//
// On a mismatch the rendered frame is written to <out-dir> for a look.
//
#include "testsupport.h"
#include "AvsPreset.h"
#include "GpuFx.h"
#include "LineMode.h"
#include "Presets.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <functional>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using namespace viz;

static constexpr int W = 320, H = 180, FRAMES = 240;   // 4 s: slow drifts like a washout show
static constexpr int TW = 16, TH = 9;
// Thumbnail tolerance, in 0..255 levels: mean over every cell and channel,
// and the worst single cell. Loose enough for other compilers; a washout or
// a black frame moves the mean by tens of levels.
static constexpr double kMeanTol = 4.0, kCellTol = 48.0;
// Simulations (fluid, reaction-diffusion) amplify rounding: another compiler
// or optimisation level gives the same kind of picture with the swirls in
// other places. For those, only the overall colour is held to account (per
// channel, so a washout or a black frame still fails) plus a loose layout.
static constexpr double kSimChannelTol = 6.0, kSimChannelRel = 0.2, kSimMeanTol = 12.0;

static bool isSimulation(const Preset& p)
{
    for (const auto& e : p.effects)
        if (e.enabled && (e.type == "fluid" || e.type == "reaction_diffusion")) return true;
    return false;
}

struct Result {
    std::string name;
    uint64_t hash = 0;
    std::vector<uint8_t> thumb;            // TW*TH*3
    double mean = 0;                       // 0..255
    bool simulation = false;
};

static uint64_t fnv(const uint8_t* p, size_t n, uint64_t h = 1469598103934665603ull)
{
    for (size_t i = 0; i < n; ++i) { h ^= p[i]; h *= 1099511628211ull; }
    return h;
}

static std::string toolchainKey()
{
#if defined(__APPLE__)
    std::string k = "darwin";
#elif defined(__linux__)
    std::string k = "linux";
#else
    std::string k = "other";
#endif
#if defined(__aarch64__) || defined(__arm64__)
    k += "-arm64";
#elif defined(__x86_64__)
    k += "-x86_64";
#endif
#if defined(__VERSION__)
    const char* v = __VERSION__;
#else
    const char* v = "unknown";
#endif
    char buf[24];
    snprintf(buf, sizeof buf, "-%08x", (unsigned)(fnv((const uint8_t*)v, strlen(v)) & 0xffffffffu));
    return k + buf;
}

// Render one preset (loaded by `load` into a fresh host) and summarise its last frame.
template <class Load>
static Result render(const std::string& name, bool smooth, Load load, std::string* err)
{
    Result r;
    r.name = name;
    drawQuality() = DrawQuality{};
    drawQuality().smooth = smooth;
    EffectHost host;
    host.resize(W, H);
    if (!load(host, err)) return r;
    VizFrame f;
    EffectContext ctx;
    for (int i = 0; i < FRAMES; ++i) {
        test::synthAudio(f, i);
        ctx.frame = (uint64_t)i;
        ctx.time = i / 60.0;
        host.renderFrame(f, ctx);
    }
    const Framebuffer& fb = host.currentSynced();
    std::vector<uint8_t> rgb((size_t)W * H * 3);
    for (int i = 0; i < W * H; ++i)
        for (int c = 0; c < 3; ++c) {
            float v = fb.px[(size_t)i * 4 + (size_t)c];
            rgb[(size_t)i * 3 + (size_t)c] = (uint8_t)std::lround(std::clamp(v, 0.f, 1.f) * 255.f);
        }
    r.hash = fnv(rgb.data(), rgb.size());
    r.thumb.assign((size_t)TW * TH * 3, 0);
    double total = 0;
    for (int ty = 0; ty < TH; ++ty)
        for (int tx = 0; tx < TW; ++tx)
            for (int c = 0; c < 3; ++c) {
                double s = 0;
                int n = 0;
                for (int y = ty * H / TH; y < (ty + 1) * H / TH; ++y)
                    for (int x = tx * W / TW; x < (tx + 1) * W / TW; ++x) {
                        s += rgb[((size_t)y * W + (size_t)x) * 3 + (size_t)c];
                        ++n;
                    }
                r.thumb[((size_t)ty * TW + (size_t)tx) * 3 + (size_t)c] = (uint8_t)std::lround(s / n);
                total += s;
            }
    r.mean = total / ((double)W * H * 3);
    return r;
}

static std::string hex(const std::vector<uint8_t>& v)
{
    static const char* d = "0123456789abcdef";
    std::string s;
    s.reserve(v.size() * 2);
    for (uint8_t b : v) { s += d[b >> 4]; s += d[b & 15]; }
    return s;
}

static std::vector<uint8_t> unhex(const std::string& s)
{
    std::vector<uint8_t> v;
    for (size_t i = 0; i + 1 < s.size(); i += 2) v.push_back((uint8_t)std::stoul(s.substr(i, 2), nullptr, 16));
    return v;
}

// "name<TAB>value" lines; '#' starts a comment.
static std::map<std::string, std::string> readTable(const fs::path& p)
{
    std::map<std::string, std::string> t;
    std::ifstream in(p);
    std::string line;
    while (std::getline(in, line)) {
        if (line.empty() || line[0] == '#') continue;
        size_t tab = line.find('\t');
        if (tab != std::string::npos) t[line.substr(0, tab)] = line.substr(tab + 1);
    }
    return t;
}

static void writePng(const std::string& dir, const std::string& name, bool smooth,
                     const std::function<bool(EffectHost&, std::string*)>& load)
{
    drawQuality() = DrawQuality{};
    drawQuality().smooth = smooth;
    EffectHost host;
    host.resize(W, H);
    std::string err;
    if (!load(host, &err)) return;
    VizFrame f;
    EffectContext ctx;
    for (int i = 0; i < FRAMES; ++i) {
        test::synthAudio(f, i);
        ctx.frame = (uint64_t)i;
        ctx.time = i / 60.0;
        host.renderFrame(f, ctx);
    }
    std::string safe;
    for (char c : name) safe += (std::isalnum((unsigned char)c) || c == '.' || c == '-') ? c : '_';
    test::writePng(host.currentSynced(), dir + "/" + safe + ".png");
}

int main(int argc, const char** argv)
{
    setvbuf(stdout, nullptr, _IOLBF, 0);
    if (argc < 4) {
        printf("usage: golden_selftest <fixture-dir> <golden-dir> <out-dir> [--update]\n");
        return 2;
    }
    const fs::path fixtures = argv[1], golden = argv[2], outDir = argv[3];
    const bool update = argc > 4 && std::strcmp(argv[4], "--update") == 0;
    const std::string backend = gpu::available() ? "metal" : "cpu";
    const std::string key = toolchainKey();
    // Exact references only mean something for the build they were made
    // with: CMake defines MSCOPES_GOLDEN_EXACT for a plain Release build (not
    // Debug, not the sanitizers, whose different code rounds differently).
#ifdef MSCOPES_GOLDEN_EXACT
    const bool exactBuild = true;
#else
    const bool exactBuild = false;
#endif
    const fs::path thumbFile = golden / (backend + ".txt");
    const fs::path hashFile = golden / (backend + "-" + key + ".hash");
    printf("golden: backend %s, toolchain %s%s\n", backend.c_str(), key.c_str(), update ? " (updating)" : "");

    // What to render: the fixtures in Authentic drawing, the built-ins in
    // Smooth, each as the app draws them by default.
    struct Job {
        std::string name;
        bool smooth;
        std::function<bool(EffectHost&, std::string*)> load;
        bool simulation = false;
    };
    std::vector<Job> jobs;
    std::vector<fs::path> files;
    for (const auto& e : fs::directory_iterator(fixtures))
        if (e.path().extension() == ".avs") files.push_back(e.path());
    std::sort(files.begin(), files.end());
    for (const auto& p : files)
        jobs.push_back({"fixture:" + p.filename().string(), false,
                        [p](EffectHost& h, std::string* err) {
                            AvsLoadReport rep;
                            bool ok = loadAvsPresetFile(h, p.string(), rep);
                            if (!ok && err) *err = rep.summary();
                            return ok;
                        }, false});
    for (const auto& pr : builtinPresets())
        jobs.push_back({"builtin:" + pr.name, true,
                        [&pr](EffectHost& h, std::string*) { applyPreset(h, pr); return true; },
                        isSimulation(pr)});

    std::vector<Result> results;
    int failures = 0;
    for (const auto& j : jobs) {
        std::string err;
        Result a = render(j.name, j.smooth, j.load, &err);
        if (a.thumb.empty()) {
            printf("FAIL %s: cannot load (%s)\n", j.name.c_str(), err.c_str());
            ++failures;
            continue;
        }
        // The same render twice must agree, or exact references mean nothing.
        Result b = render(j.name, j.smooth, j.load, &err);
        if (a.hash != b.hash) {
            printf("FAIL %s: two identical renders differ (nondeterministic)\n", j.name.c_str());
            ++failures;
        }
        a.simulation = j.simulation;
        results.push_back(a);
    }

    if (update) {
        fs::create_directories(golden);
        std::ofstream t(thumbFile);
        t << "# golden_selftest thumbnails (" << TW << "x" << TH << " RGB means, 0..255), backend "
          << backend << ", " << W << "x" << H << ", " << FRAMES << " frames of the synthetic test audio\n";
        for (const auto& r : results) t << r.name << '\t' << hex(r.thumb) << '\n';
        if (exactBuild) {
            std::ofstream h(hashFile);
            h << "# golden_selftest exact hashes, backend " << backend << ", toolchain " << key << "\n";
            for (const auto& r : results) {
                char buf[24];
                snprintf(buf, sizeof buf, "%016llx", (unsigned long long)r.hash);
                h << r.name << '\t' << buf << '\n';
            }
        }
        printf("golden: wrote %zu references to %s%s%s\n", results.size(), thumbFile.filename().c_str(),
               exactBuild ? " and " : " (no exact hashes: not a plain Release build)",
               exactBuild ? hashFile.filename().c_str() : "");
        return failures ? 1 : 0;
    }

    const auto thumbs = readTable(thumbFile);
    const auto hashes = exactBuild ? readTable(hashFile) : std::map<std::string, std::string>{};
    if (thumbs.empty()) {
        printf("FAIL no thumbnail references in %s (run with --update)\n", thumbFile.c_str());
        return 1;
    }
    if (!exactBuild)
        printf("golden: not a plain Release build; checking thumbnails only\n");
    else if (hashes.empty())
        printf("golden: no exact references for this toolchain; checking thumbnails only\n");
    int exact = 0;
    fs::create_directories(outDir);
    for (const auto& r : results) {
        bool bad = false;
        auto it = thumbs.find(r.name);
        if (it == thumbs.end()) {
            printf("FAIL %s: no reference (new preset? run with --update)\n", r.name.c_str());
            bad = true;
        } else {
            const std::vector<uint8_t> ref = unhex(it->second);
            double sum = 0, worst = 0, refMean = 0;
            for (size_t i = 0; i < ref.size() && i < r.thumb.size(); ++i) {
                double d = std::fabs((double)ref[i] - (double)r.thumb[i]);
                sum += d;
                worst = std::max(worst, d);
                refMean += ref[i];
            }
            const double mean = sum / (double)ref.size();
            refMean /= (double)ref.size();
            if (ref.size() != r.thumb.size()) {
                printf("FAIL %s: reference has the wrong size (run with --update)\n", r.name.c_str());
                bad = true;
            } else if (r.simulation) {
                // Overall colour per channel, and a loose layout.
                double refC[3] = {}, curC[3] = {};
                for (size_t i = 0; i < ref.size(); ++i) { refC[i % 3] += ref[i]; curC[i % 3] += r.thumb[i]; }
                const double cells = (double)(ref.size() / 3);
                for (int c = 0; c < 3 && !bad; ++c) {
                    refC[c] /= cells; curC[c] /= cells;
                    const double tol = std::max(kSimChannelTol, refC[c] * kSimChannelRel);
                    if (std::fabs(refC[c] - curC[c]) > tol) {
                        printf("FAIL %s: colour changed (%s channel %.1f -> %.1f levels; simulation, compared loosely)\n",
                               r.name.c_str(), c == 0 ? "red" : c == 1 ? "green" : "blue", refC[c], curC[c]);
                        bad = true;
                    }
                }
                if (!bad && mean > kSimMeanTol) {
                    printf("FAIL %s: picture changed (mean difference %.1f levels; simulation, compared loosely)\n",
                           r.name.c_str(), mean);
                    bad = true;
                }
            } else if (mean > kMeanTol || worst > kCellTol) {
                printf("FAIL %s: picture changed (mean difference %.1f levels, worst cell %.0f; brightness %.1f -> %.1f)\n",
                       r.name.c_str(), mean, worst, refMean, r.mean);
                bad = true;
            }
        }
        auto h = hashes.find(r.name);
        if (!bad && h != hashes.end()) {
            char buf[24];
            snprintf(buf, sizeof buf, "%016llx", (unsigned long long)r.hash);
            if (h->second != buf) {
                printf("FAIL %s: not byte-identical to the reference made with this toolchain\n", r.name.c_str());
                bad = true;
            } else {
                ++exact;
            }
        }
        if (bad) {
            ++failures;
            for (const auto& j : jobs)
                if (j.name == r.name) writePng(outDir.string(), j.name, j.smooth, j.load);
        }
    }
    printf("golden: %zu presets, %d byte-identical, %d failed%s\n", results.size(), exact, failures,
           failures ? " (rendered frames written to the output folder)" : "");
    return failures ? 1 : 0;
}
