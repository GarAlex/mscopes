//
// beat_selftest.cpp — end-to-end check of Analyzer + BeatDetector on synthetic
// PCM: a 128 BPM kick pattern under noise and a pad, then silence, then a
// steady (beatless) bass tone. Exits non-zero if beats are missed, invented,
// or the tempo estimate is off.
//
// Build/run via tools/run-tests.sh, or:
//   built by CMake (core/tests), or tools/run-tests.sh beat
//
// (portable: no platform headers)
#include "Analyzer.h"
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <vector>

using namespace viz;

static constexpr int    kRate    = 48000;
static constexpr int    kHop     = kRate / 60;          // one analysis per 1/60 s
static constexpr double kBpm     = 128.0;
static constexpr double kBeatSec = 60.0 / kBpm;         // 0.46875 s

struct Section { const char* name; double seconds; int kind; }; // 0 kicks, 1 silence, 2 steady

static float frand() { return (float)rand() / (float)RAND_MAX * 2.f - 1.f; }

int main()
{
    srand(1234);
    Analyzer an;

    const Section sections[] = {
        { "kicks @128 BPM + noise + pad", 20.0, 0 },
        { "silence",                      5.0, 1 },
        { "steady 60 Hz bass, no hits",   10.0, 2 },
        { "kicks again (re-lock)",        10.0, 0 },
    };

    std::vector<double> kickTimes, beatTimes;
    std::vector<double> bpmSamples;                 // bpm reported during kick sections
    float rmsKick = 0.f, rmsSilence = 0.f, rmsSteady = 0.f;
    int   nSilenceBeats = 0, nSteadyBeats = 0;
    double t = 0.0;                                 // synthesis clock
    int sampleIdx = 0;

    std::vector<float> buf(kHop * 2);
    for (const Section& sec : sections) {
        double secStart = t;
        double nextKick = secStart;
        double lastKick = -1.0;
        float  secPeakRms = 0.f;
        int    secBeats = 0;
        while (t < secStart + sec.seconds) {
            for (int n = 0; n < kHop; ++n, ++sampleIdx) {
                double ts = (double)sampleIdx / kRate;
                float v = 0.f;
                if (sec.kind == 0) {
                    if (ts >= nextKick) { kickTimes.push_back(nextKick); lastKick = nextKick; nextKick += kBeatSec; }
                    if (lastKick >= 0) {
                        double age = ts - lastKick;
                        // 60 Hz kick with a fast pitch drop, exponential decay
                        v += 0.6f * (float)(std::exp(-age / 0.09) * std::sin(2 * M_PI * (60.0 + 40.0 * std::exp(-age / 0.02)) * age));
                    }
                    v += 0.10f * (float)std::sin(2 * M_PI * 440.0 * ts);     // pad (mid band)
                    v += 0.02f * frand();                                     // hiss
                } else if (sec.kind == 1) {
                    v = 0.0005f * frand();                                    // dither-level noise
                } else {
                    v = 0.30f * (float)std::sin(2 * M_PI * 60.0 * ts) + 0.01f * frand();
                }
                buf[n * 2] = buf[n * 2 + 1] = v;
            }
            an.push(buf.data(), kHop, 2);
            VizFrame f;
            an.analyze(f, (double)kHop / kRate);   // offline: explicit hop, not wall clock
            t += (double)kHop / kRate;
            secPeakRms = std::max(secPeakRms, an.lastBassRms());
            if (f.beat) { beatTimes.push_back(t); ++secBeats; }
            if (sec.kind == 0 && f.bpm > 0.f && t - secStart > 5.0) bpmSamples.push_back(f.bpm);
        }
        if (sec.kind == 0) rmsKick = std::max(rmsKick, secPeakRms);
        if (sec.kind == 1) { rmsSilence = secPeakRms; nSilenceBeats = secBeats; }
        if (sec.kind == 2) { rmsSteady = secPeakRms; nSteadyBeats = secBeats; }
        printf("  %-32s beats=%d peakBassRms=%.4f\n", sec.name, secBeats, secPeakRms);
    }

    // --- scoring ---
    // The detector runs one hop late at most (analysis is of the trailing
    // window), so allow generous but bounded tolerance.
    const double tol = 0.06;
    int hits = 0, misses = 0;
    for (double k : kickTimes) {
        bool found = false;
        for (double b : beatTimes) if (std::fabs(b - k) <= tol) { found = true; break; }
        if (found) ++hits; else ++misses;
    }
    int falsePositives = 0;
    for (double b : beatTimes) {
        bool near = false;
        for (double k : kickTimes) if (std::fabs(b - k) <= tol) { near = true; break; }
        if (!near) ++falsePositives;
    }
    // Allow the warm-up window to miss the very first kick or two.
    double bpmMean = 0.0;
    for (double b : bpmSamples) bpmMean += b;
    bpmMean = bpmSamples.empty() ? 0.0 : bpmMean / bpmSamples.size();
    int bpmOff = 0;
    for (double b : bpmSamples) if (std::fabs(b - kBpm) > 4.0) ++bpmOff;

    printf("kicks=%zu hits=%d misses=%d false+=%d | silence beats=%d | steady beats=%d | "
           "bpm mean=%.1f (off-by->4: %d/%zu) | rms kick=%.4f silence=%.5f steady=%.4f\n",
           kickTimes.size(), hits, misses, falsePositives, nSilenceBeats, nSteadyBeats,
           bpmMean, bpmOff, bpmSamples.size(), rmsKick, rmsSilence, rmsSteady);

    int fails = 0;
    auto check = [&](bool ok, const char* what) {
        printf("  [%s] %s\n", ok ? "ok" : "FAIL", what);
        if (!ok) ++fails;
    };
    check(misses <= 2,                       "every kick detected (≤2 warm-up misses)");
    check(falsePositives <= 2,               "no invented beats between kicks (≤2)");
    check(nSilenceBeats == 0,                "silence produces no beats");
    check(nSteadyBeats <= 1,                 "steady tone produces no beats after its onset");
    check(!bpmSamples.empty() && std::fabs(bpmMean - kBpm) <= 2.0, "BPM estimate within ±2 of 128");
    check(bpmOff * 10 <= (int)bpmSamples.size(), "BPM stable (≤10% samples off by >4)");
    check(rmsSilence < an.beatDetector().silenceFloor, "silence sits below the silence floor");

    printf("beat_selftest: %s\n", fails ? "FAILED" : "PASS");
    return fails ? 1 : 0;
}
