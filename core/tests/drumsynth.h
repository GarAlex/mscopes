//
// drumsynth.h — synthetic kick, snare and hi-hat for the feature tests and
// tuning: deterministic, with the hit times known exactly.
//
#pragma once
#include <cmath>
#include <random>
#include <vector>

namespace viz { namespace test {

static constexpr double kDrumSr = 48000.0;

// 5 ms raised-cosine fades at both ends: real notes don't start or stop
// with a click (and a click is a broadband onset of its own).
inline double fade(double t, double len)
{
    const double f = 0.005;
    double g = 1.0;
    if (t < f) g *= 0.5 - 0.5 * std::cos(M_PI * t / f);
    if (len - t < f) g *= 0.5 - 0.5 * std::cos(M_PI * std::max(0.0, len - t) / f);
    return g;
}

inline void addTone(std::vector<float>& s, double hz, double amp, double t0 = 0, double t1 = 1e9)
{
    for (size_t i = 0; i < s.size(); ++i) {
        double t = (double)i / kDrumSr;
        if (t >= t0 && t < t1) {
            double g = (t1 < 1e8) ? fade(t - t0, t1 - t0) : 1.0;
            s[i] += (float)(amp * g * std::sin(2 * M_PI * hz * t));
        }
    }
}

// Fade over the last 5 ms of a sound `len` seconds long.
inline double tail(double t, double len)
{
    return len - t < 0.005 ? 0.5 - 0.5 * std::cos(M_PI * std::max(0.0, len - t) / 0.005) : 1.0;
}

struct Drums {
    std::vector<float> sig;
    std::vector<double> kicks, snares, hats;
};

inline void addKick(std::vector<float>& s, double t0, double amp)
{
    double ph = 0;
    for (int i = 0; i < (int)(0.45 * kDrumSr); ++i) {
        size_t n = (size_t)(t0 * kDrumSr) + (size_t)i;
        if (n >= s.size()) break;
        double t = i / kDrumSr;
        double f = 50 + 110 * std::exp(-t / 0.03);
        ph += 2 * M_PI * f / kDrumSr;
        s[n] += (float)(amp * 0.8 * std::exp(-t / 0.12) * std::sin(ph) * tail(t, 0.45));
    }
}

inline void addSnare(std::vector<float>& s, double t0, double amp, std::mt19937& rng)
{
    std::uniform_real_distribution<float> u(-1.f, 1.f);
    float prev = 0.f, lp = 0.f;
    const float a = 1.f - std::exp(-2.f * (float)M_PI * 7000.f / (float)kDrumSr);
    for (int i = 0; i < (int)(0.35 * kDrumSr); ++i) {
        size_t n = (size_t)(t0 * kDrumSr) + (size_t)i;
        if (n >= s.size()) break;
        double t = i / kDrumSr;
        float w = u(rng);
        float hp = 0.5f * (w - prev); prev = w;           // wires: noise above the body...
        lp += a * (hp - lp);                              // ...rolling off past ~7 kHz
        double body = 0.35 * std::exp(-t / 0.05) * std::sin(2 * M_PI * 185 * t);
        s[n] += (float)(amp * (body + 0.9 * std::exp(-t / 0.08) * lp) * tail(t, 0.35));
    }
}

inline void addHat(std::vector<float>& s, double t0, double amp, std::mt19937& rng)
{
    std::uniform_real_distribution<float> u(-1.f, 1.f);
    float p1 = 0.f, p2 = 0.f;
    for (int i = 0; i < (int)(0.12 * kDrumSr); ++i) {
        size_t n = (size_t)(t0 * kDrumSr) + (size_t)i;
        if (n >= s.size()) break;
        double t = i / kDrumSr;
        float w = u(rng);
        float hp = (w - 2 * p1 + p2) * 0.25f;             // second difference: bright
        p2 = p1; p1 = w;
        s[n] += (float)(amp * 0.5 * std::exp(-t / 0.025) * hp * tail(t, 0.12));
    }
}

// 120 BPM, 8 s: kicks on 1 and 3, snares on 2 and 4, hats on every eighth,
// over a sustained bass line that changes note with the kick each bar.
inline Drums makeDrums(bool kick, bool snare, bool hat, double gain, bool bassline)
{
    Drums d;
    d.sig.assign((size_t)(kDrumSr * 8.5), 0.f);
    std::mt19937 rng(42);
    for (int beat = 0; beat < 16; ++beat) {
        double t = 0.25 + beat * 0.5;
        if (kick && beat % 2 == 0) { addKick(d.sig, t, gain); d.kicks.push_back(t); }
        if (snare && beat % 2 == 1) { addSnare(d.sig, t, gain, rng); d.snares.push_back(t); }
        if (hat) for (int e = 0; e < 2; ++e) {
            addHat(d.sig, t + e * 0.25, gain, rng);
            d.hats.push_back(t + e * 0.25);
        }
    }
    if (bassline)
        for (int bar = 0; bar < 4; ++bar)
            addTone(d.sig, bar % 2 ? 73.4 : 55.0, 0.12 * gain, 0.25 + bar * 2.0, 0.25 + bar * 2.0 + 1.9);
    return d;
}

}} // namespace viz::test
