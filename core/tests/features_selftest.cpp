//
// features_selftest.cpp — the audio features (audio/Features.h) on
// synthetic audio fed through the real Analyzer, the way the app feeds it:
// blocks of samples per display frame, then analyze().
//
//   bands     a tone lights its own band; per-band gain lifts a quiet band;
//             silence stays dark; attack is fast, release is smooth
//   drums     synthetic kick / snare / hi-hat, solo and mixed: each detector
//             finds its own instrument and mostly ignores the others; the
//             result does not depend on loudness or frame rate
//   scope     a sawtooth with a non-integer period holds still in the
//             triggered waveform while the raw waveform slides
//   recent    every sample arrives exactly once, in order
//   classic   the classic fields are what they were without the features
//
#include "Analyzer.h"
#include "AudioVars.h"
#include "EffectHost.h"
#include "ModernEffects.h"
#include "Presets.h"
#include "drumsynth.h"
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <random>
#include <vector>

using namespace viz;
using namespace viz::test;

static int gFail = 0;
#define CHECK(cond, ...) do { if (!(cond)) { std::printf("  FAIL: " __VA_ARGS__); std::printf("\n"); ++gFail; } } while (0)

static constexpr double kSr = 48000.0;

// Runs `sig` (mono, duplicated to stereo) through an Analyzer in blocks of
// sr/fps samples; calls `each` after every analyze().
static void run(const std::vector<float>& sig, double fps,
                const std::function<void(const VizFrame&, double tEnd)>& each)
{
    Analyzer an;
    an.setSampleRate(kSr);
    const int block = (int)std::lround(kSr / fps);
    std::vector<float> inter;
    VizFrame f;
    for (size_t pos = 0; pos + (size_t)block <= sig.size(); pos += (size_t)block) {
        inter.resize((size_t)block * 2);
        for (int i = 0; i < block; ++i) inter[(size_t)i * 2] = inter[(size_t)i * 2 + 1] = sig[pos + (size_t)i];
        an.push(inter.data(), block, 2);
        f.clear();
        an.analyze(f, 1.0 / fps);
        each(f, (double)(pos + (size_t)block) / kSr);
    }
}

static int bandOf(double hz)
{
    for (int b = 0; b < kBands; ++b)
        if (hz >= VizFrame::bandEdgeHz(b) && hz < VizFrame::bandEdgeHz(b + 1)) return b;
    return -1;
}

// ---------------------------------------------------------------------------
static void testBands()
{
    std::printf("bands\n");
    // A 1 kHz tone lights its band and leaves far bands dark.
    {
        std::vector<float> s((size_t)(kSr * 2), 0.f);
        addTone(s, 1000, 0.1);
        VizFrame last;
        run(s, 60, [&](const VizFrame& f, double) { last = f; });
        const int b = bandOf(1000);
        CHECK(last.hasFeatures, "features not filled");
        CHECK(last.bands[b] > 0.85f, "1 kHz band %.2f", last.bands[b]);
        CHECK(last.bands[bandOf(60)] < 0.1f && last.bands[bandOf(10000)] < 0.1f,
              "far bands lit: 60 Hz %.2f, 10 kHz %.2f", last.bands[bandOf(60)], last.bands[bandOf(10000)]);
        std::printf("  1 kHz: own band %.2f, 60 Hz %.2f, 10 kHz %.2f\n",
                    last.bands[b], last.bands[bandOf(60)], last.bands[bandOf(10000)]);
    }
    // Per-band gain: a treble tone 20 dB below the bass reads nearly as high.
    {
        std::vector<float> s((size_t)(kSr * 3), 0.f);
        addTone(s, 100, 0.3);
        addTone(s, 8000, 0.03);
        VizFrame last;
        run(s, 60, [&](const VizFrame& f, double) { last = f; });
        float lo = last.bands[bandOf(100)], hi = last.bands[bandOf(8000)];
        CHECK(lo > 0.85f && hi > 0.75f, "per-band gain: 100 Hz %.2f, 8 kHz (-20 dB) %.2f", lo, hi);
        std::printf("  100 Hz %.2f, 8 kHz at -20 dB %.2f\n", lo, hi);
    }
    // Silence stays dark, and nothing fires.
    {
        std::vector<float> s((size_t)(kSr * 2), 0.f);
        std::mt19937 rng(7);
        std::normal_distribution<float> nd(0.f, 1e-5f);        // -100 dBFS hiss
        for (auto& v : s) v = nd(rng);
        float maxBand = 0.f, maxLevel = 0.f; int hits = 0;
        run(s, 60, [&](const VizFrame& f, double) {
            for (float v : f.bands) maxBand = std::max(maxBand, v);
            maxLevel = std::max(maxLevel, f.level);
            hits += f.kickHit + f.snareHit + f.hatHit;
        });
        CHECK(maxBand < 0.05f && maxLevel < 0.05f && hits == 0,
              "silence: band %.3f level %.3f hits %d", maxBand, maxLevel, hits);
    }
    // Attack within a few frames; release smooth, not instant.
    {
        std::vector<float> s((size_t)(kSr * 3), 0.f);
        addTone(s, 500, 0.2, 1.0, 2.0);
        const int b = bandOf(500);
        float at50ms = 0.f, at110msAfter = 0.f, at500msAfter = 1.f;
        run(s, 60, [&](const VizFrame& f, double t) {
            if (std::fabs(t - 1.05) < 0.009) at50ms = f.bands[b];
            if (std::fabs(t - 2.11) < 0.009) at110msAfter = f.bands[b];
            if (std::fabs(t - 2.50) < 0.009) at500msAfter = f.bands[b];
        });
        CHECK(at50ms > 0.8f, "attack: %.2f 50 ms after onset", at50ms);
        CHECK(at110msAfter > 0.25f && at110msAfter < 0.75f, "release: %.2f 110 ms after stop", at110msAfter);
        CHECK(at500msAfter < 0.1f, "release: %.2f 500 ms after stop", at500msAfter);
        std::printf("  attack %.2f @50ms; release %.2f @110ms, %.2f @500ms\n", at50ms, at110msAfter, at500msAfter);
    }
}

struct Score { int found = 0, total = 0, extra = 0; };

// A detection in the frame ending at t matches an onset in [t - 75 ms, t].
static Score score(const std::vector<double>& truth, const std::vector<double>& det)
{
    Score s;
    s.total = (int)truth.size();
    std::vector<bool> used(truth.size(), false);
    for (double t : det) {
        bool ok = false;
        for (size_t i = 0; i < truth.size(); ++i)
            if (!used[i] && truth[i] <= t + 1e-9 && truth[i] >= t - 0.075) { used[i] = true; ok = true; break; }
        if (!ok) {
            s.extra++;
            if (std::getenv("FEATURES_VERBOSE")) std::printf("      extra at %.3f s\n", t);
        }
    }
    for (bool u : used) s.found += u;
    return s;
}

struct Detections { std::vector<double> kick, snare, hat; };

static Detections detect(const std::vector<float>& sig, double fps)
{
    Detections d;
    run(sig, fps, [&](const VizFrame& f, double t) {
        if (f.kickHit) d.kick.push_back(t);
        if (f.snareHit) d.snare.push_back(t);
        if (f.hatHit) d.hat.push_back(t);
    });
    return d;
}

static void testDrums()
{
    std::printf("drums\n");
    auto report = [](const char* what, const char* det, const Score& s) {
        std::printf("  %-12s %-6s %2d/%2d found, %d extra\n", what, det, s.found, s.total, s.extra);
    };
    {   // Solo instruments: each detector finds its own, the others stay quiet.
        Drums k = makeDrums(true, false, false, 1.0, false);
        Detections dk = detect(k.sig, 60);
        Score s = score(k.kicks, dk.kick);
        report("kick solo", "kick", s);
        CHECK(s.found >= 7 && s.extra <= 1, "kick solo: kick %d/%d, %d extra", s.found, s.total, s.extra);
        CHECK(dk.snare.size() <= 1 && dk.hat.size() <= 1, "kick solo: snare %zu hat %zu", dk.snare.size(), dk.hat.size());

        Drums sn = makeDrums(false, true, false, 1.0, false);
        Detections ds = detect(sn.sig, 60);
        s = score(sn.snares, ds.snare);
        report("snare solo", "snare", s);
        CHECK(s.found >= 7 && s.extra <= 1, "snare solo: snare %d/%d, %d extra", s.found, s.total, s.extra);
        CHECK(ds.kick.size() <= 1 && ds.hat.size() <= 2, "snare solo: kick %zu hat %zu", ds.kick.size(), ds.hat.size());

        Drums h = makeDrums(false, false, true, 1.0, false);
        Detections dh = detect(h.sig, 60);
        s = score(h.hats, dh.hat);
        report("hat solo", "hat", s);
        CHECK(s.found >= 29 && s.extra <= 2, "hat solo: hat %d/%d, %d extra", s.found, s.total, s.extra);
        CHECK(dh.kick.empty() && dh.snare.size() <= 3, "hat solo: kick %zu snare %zu", dh.kick.size(), dh.snare.size());
    }
    // The full kit over a bass line, at three loudnesses and two frame rates.
    for (double gain : {1.0, 0.1, 0.02})
        for (double fps : {60.0, 30.0}) {
            if (fps == 30.0 && gain != 1.0) continue;
            Drums d = makeDrums(true, true, true, gain, true);
            Detections dd = detect(d.sig, fps);
            Score sk = score(d.kicks, dd.kick), ss = score(d.snares, dd.snare), sh = score(d.hats, dd.hat);
            char what[48];
            std::snprintf(what, sizeof what, "kit %+.0fdB %.0ffps", 20 * std::log10(gain), fps);
            report(what, "kick", sk); report("", "snare", ss); report("", "hat", sh);
            CHECK(sk.found >= 7 && sk.extra <= 1, "%s: kick %d/%d, %d extra", what, sk.found, sk.total, sk.extra);
            CHECK(ss.found >= 7 && ss.extra <= 1, "%s: snare %d/%d, %d extra", what, ss.found, ss.total, ss.extra);
            // A hat played with a snare is masked by the snare's own top
            // (8 of the 32): the rest must be found.
            CHECK(sh.found >= 23 && sh.extra <= 2, "%s: hat %d/%d, %d extra", what, sh.found, sh.total, sh.extra);
        }
    {   // Envelopes: 1 right after a kick, decaying.
        Drums k = makeDrums(true, false, false, 1.0, false);
        float atHit = 0.f, later = 1.f;
        double hitT = -1;
        run(k.sig, 60, [&](const VizFrame& f, double t) {
            if (f.kickHit && hitT < 0) { hitT = t; atHit = f.kick; }
            if (hitT > 0 && std::fabs(t - (hitT + 0.3)) < 0.009) later = f.kick;
        });
        CHECK(atHit > 0.7f && later < 0.25f, "kick envelope: %.2f at the hit, %.2f 300 ms later", atHit, later);
    }
}

// ---------------------------------------------------------------------------
static void testScope()
{
    std::printf("scope\n");
    // Sawtooth at 220 Hz (218.18 samples a period) plus its octave: the
    // raw waveform slides every frame, the triggered one holds.
    std::vector<float> s((size_t)(kSr * 3), 0.f);
    for (size_t i = 0; i < s.size(); ++i) {
        double t = (double)i / kSr;
        double saw = 2.0 * (220.0 * t - std::floor(220.0 * t + 0.5));
        s[i] = (float)(0.4 * saw + 0.2 * std::sin(2 * M_PI * 440 * t + 0.7));
    }
    std::vector<float> prevScope(kWaveformSamples), prevWave(kWaveformSamples);
    double dScope = 0, dWave = 0; int n = 0;
    bool have = false;
    run(s, 60, [&](const VizFrame& f, double t) {
        if (t > 1.0) {
            if (have) {
                double a = 0, b = 0;
                for (int i = 0; i < kWaveformSamples; ++i) {
                    a += std::fabs(f.scope[0][i] - prevScope[(size_t)i]);
                    b += std::fabs(f.waveform[0][i] - prevWave[(size_t)i]);
                }
                dScope += a / kWaveformSamples; dWave += b / kWaveformSamples; ++n;
            }
            have = true;
        }
        std::copy(f.scope[0], f.scope[0] + kWaveformSamples, prevScope.begin());
        std::copy(f.waveform[0], f.waveform[0] + kWaveformSamples, prevWave.begin());
    });
    dScope /= n; dWave /= n;
    std::printf("  frame-to-frame change: triggered %.4f, raw %.4f\n", dScope, dWave);
    CHECK(dScope < 0.02 && dWave > 0.1, "triggered %.4f raw %.4f", dScope, dWave);
}

// ---------------------------------------------------------------------------
static void testRecent()
{
    std::printf("recent\n");
    std::vector<float> s((size_t)(kSr * 1), 0.f);
    for (size_t i = 0; i < s.size(); ++i) s[i] = (float)((int)(i % 1000) - 500) / 1000.f;
    for (double fps : {60.0, 24.0}) {
        std::vector<float> got;
        run(s, fps, [&](const VizFrame& f, double) { got.insert(got.end(), f.recent[0], f.recent[0] + f.recentCount); });
        size_t block = (size_t)std::lround(kSr / fps);
        size_t expect = (s.size() / block) * block;
        bool same = got.size() == expect;
        for (size_t i = 0; same && i < got.size(); ++i) same = got[i] == s[i];
        CHECK(same, "%.0f fps: %zu samples back of %zu, in order: %d", fps, got.size(), expect, (int)same);
    }
}

// ---------------------------------------------------------------------------
//  The classic fields are untouched by the features: an Analyzer's output
//  for them matches a reference computed the old way (spectrum/waveform of
//  the newest window, computeBands, the beat detector on the bass RMS).
static void testClassicUnchanged()
{
    std::printf("classic\n");
    Drums d = makeDrums(true, true, true, 1.0, true);
    // Two analyzers: one normal, one whose features see a different sample
    // rate. Classic fields must not depend on anything feature-related.
    Analyzer a, b;
    a.setSampleRate(48000); b.setSampleRate(22050);
    const int block = 800;
    std::vector<float> inter((size_t)block * 2);
    VizFrame fa, fb;
    int diffs = 0, beatsA = 0;
    for (size_t pos = 0; pos + block <= d.sig.size(); pos += block) {
        for (int i = 0; i < block; ++i) inter[(size_t)i * 2] = inter[(size_t)i * 2 + 1] = d.sig[pos + (size_t)i];
        a.push(inter.data(), block, 2); b.push(inter.data(), block, 2);
        fa.clear(); fb.clear();
        a.analyze(fa, 1.0 / 60); b.analyze(fb, 1.0 / 60);
        beatsA += fa.beat;
        if (std::memcmp(fa.spectrum, fb.spectrum, sizeof fa.spectrum) || std::memcmp(fa.waveform, fb.waveform, sizeof fa.waveform)
            || fa.bass != fb.bass || fa.mid != fb.mid || fa.treble != fb.treble || fa.beat != fb.beat || fa.bpm != fb.bpm)
            ++diffs;
    }
    CHECK(diffs == 0, "classic fields differ in %d frames", diffs);
    CHECK(beatsA > 4, "classic beat detector saw %d beats", beatsA);
}

// ---------------------------------------------------------------------------
//  Script variables: a script that reads kick gets the feature; a script
//  that assigns kick (an AVS preset's own variable) keeps its own value.
static void testScriptVars()
{
    std::printf("script variables\n");
    VizFrame f;
    f.hasFeatures = true;
    f.kick = 0.5f; f.snare = 0.25f; f.level = 0.8f; f.trebleAtt = 0.3f;

    eel::VM reader;
    eel::Program pr = eel::compile(reader, "a = kick * 2; b = snare + level; c = treb_att;");
    AudioVars rv; rv.bind(reader, {&pr});
    rv.set(f); pr.run();
    CHECK(*reader.var("a") == 1.0 && std::fabs(*reader.var("b") - 1.05) < 1e-6 && std::fabs(*reader.var("c") - 0.3) < 1e-6,
          "reader: a=%g b=%g c=%g", *reader.var("a"), *reader.var("b"), *reader.var("c"));

    eel::VM owner;
    eel::Program init = eel::compile(owner, "kick = 10;");
    eel::Program frame = eel::compile(owner, "kick = kick + 1; assign(hat, 7);");
    AudioVars ov; ov.bind(owner, {&init, &frame});
    init.run();
    for (int i = 0; i < 3; ++i) { ov.set(f); frame.run(); }
    CHECK(*owner.var("kick") == 13.0 && *owner.var("hat") == 7.0,
          "owner: kick=%g (want 13) hat=%g (want 7)", *owner.var("kick"), *owner.var("hat"));
    CHECK(*owner.var("snare") == 0.25, "owner still gets the names it doesn't assign: snare=%g", *owner.var("snare"));

    VizFrame plain;                                 // a source without features
    plain.beat = true; plain.bass = 0.6f;
    rv.set(plain); pr.run();
    CHECK(*reader.var("a") == 2.0, "without features kick follows the beat: a=%g", *reader.var("a"));
}

// The trigger parameter picks which hit sets an effect off.
static void testTriggers()
{
    std::printf("triggers\n");
    VectorscopeEffect fx;                            // any effect: onBeat lives on Effect
    VizFrame f;
    f.hasFeatures = true; f.beat = true;
    fx.trigger = 0; CHECK(fx.onBeat(f), "trigger beat");
    fx.trigger = 2; CHECK(!fx.onBeat(f), "trigger snare fired on the beat alone");
    f.snareHit = true; CHECK(fx.onBeat(f), "trigger snare missed a snare");
    fx.trigger = 1; CHECK(!fx.onBeat(f), "trigger kick fired on a snare");
    VizFrame plain; plain.beat = true;
    fx.trigger = 3; CHECK(fx.onBeat(plain), "without features every trigger is the beat");
}

// The vectorscope draws every sample; silence draws nothing.
static void testVectorscope()
{
    std::printf("vectorscope\n");
    for (int mode = 0; mode < 2; ++mode) {
        EffectHost host;
        host.resize(320, 240);
        auto vs = effectRegistry().at("vectorscope")();
        vs->setParam("mode", (float)mode);
        host.add(std::move(vs));
        VizFrame f;
        EffectContext ctx;
        double lit = 0, silentLit = 0;
        for (int i = 0; i < 20; ++i) {
            f.clear();
            f.hasFeatures = true;
            f.recentCount = 800;
            for (int k = 0; k < 800; ++k) {
                double t = (i * 800 + k) / 48000.0;
                f.recent[0][k] = (float)(0.4 * std::sin(2 * M_PI * 220 * t));
                f.recent[1][k] = (float)(0.4 * std::cos(2 * M_PI * 220 * t));
            }
            ctx.frame = (uint64_t)i; ctx.time = i / 60.0;
            host.clearCanvas();
            host.renderFrame(f, ctx);
        }
        const Framebuffer& fb = host.currentSynced();
        for (int y = 0; y < fb.h; ++y) for (int x = 0; x < fb.w; ++x) lit += fb.at(x, y)[1] > 0.05f;
        // then silence
        for (int i = 0; i < 5; ++i) {
            f.clear(); f.hasFeatures = true; f.recentCount = 800;
            host.clearCanvas();
            host.renderFrame(f, ctx);
        }
        const Framebuffer& fb2 = host.currentSynced();
        for (int y = 0; y < fb2.h; ++y) for (int x = 0; x < fb2.w; ++x) silentLit += fb2.at(x, y)[1] > 0.05f;
        CHECK(lit > 200 && silentLit == 0, "mode %d: %g lit pixels with a tone, %g in silence", mode, lit, silentLit);
        std::printf("  mode %d: %.0f lit pixels\n", mode, lit);
    }
}

// Every built-in preset on analyzer frames (drums, bass): no crash, finite,
// something on screen.
static void testPresetsOnFeatures()
{
    std::printf("presets on features\n");
    Drums d = makeDrums(true, true, true, 1.0, true);
    for (const auto& p : builtinPresets()) {
        EffectHost host;
        host.resize(320, 180);
        applyPreset(host, p);
        EffectContext ctx;
        int i = 0;
        run(d.sig, 60, [&](const VizFrame& f, double t) {
            if (t > 4.0) return;
            ctx.frame = (uint64_t)i; ctx.time = i / 60.0; ++i;
            host.renderFrame(f, ctx);
        });
        const Framebuffer& fb = host.currentSynced();
        double sum = 0; bool finite = true;
        for (int y = 0; y < fb.h; ++y) for (int x = 0; x < fb.w; ++x)
            for (int c = 0; c < 3; ++c) { float v = fb.at(x, y)[c]; finite &= std::isfinite(v); sum += v; }
        CHECK(finite && sum > 1.0, "%s: finite %d, energy %.1f", p.name.c_str(), (int)finite, sum);
    }
}

int main()
{
    testBands();
    testDrums();
    testScope();
    testRecent();
    testClassicUnchanged();
    testScriptVars();
    testTriggers();
    testVectorscope();
    testPresetsOnFeatures();
    if (gFail) { std::printf("features_selftest: %d FAILED\n", gFail); return 1; }
    std::printf("features_selftest: ALL PASS\n");
    return 0;
}
