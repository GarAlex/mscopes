//
// Analyzer.h — turns raw interleaved float PCM into a shared viz::VizFrame.
//
// Standalone-only: the Music plugin gets a spectrum from Music already, but for
// arbitrary system audio we must run our own FFT. Thread model: the audio
// callback calls push() (real-time thread); a timer on the main thread calls
// analyze() to produce a frame. The sample ring is lock-free (single
// producer, single consumer): the audio thread never blocks.
// Portable C++: the FFT comes from platform/fft.h (vDSP on Apple, our own
// radix-2 elsewhere).
//
#pragma once
#include "VizFrame.h"
#include "BeatDetector.h"
#include "../platform/fft.h"
#include <atomic>
#include <chrono>
#include <vector>
#include <cstdint>

namespace viz {

class Analyzer {
public:
    Analyzer();
    ~Analyzer();

    // Called from an audio thread. Interleaved float frames, `channels` wide.
    // Up to kSlots independent sources (one per tapped process) are summed
    // by analyze(); each slot is single-producer. push() without a slot is
    // slot 0.
    static constexpr int kSlots = 8;
    void push(const float* interleaved, int numFrames, int channels) { push(0, interleaved, numFrames, channels); }
    void push(int slot, const float* interleaved, int numFrames, int channels);

    // Called from the main/timer thread. Fills `out` from the latest audio:
    // waveform, spectrum, bands, and beat/bpm/beatPhase from the detector.
    // `dt` is the seconds elapsed since the previous analyze(); pass <= 0
    // (the default) to measure it from the wall clock. Offline callers (tests,
    // renders faster than real time) must pass their hop size explicitly.
    void analyze(VizFrame& out, double dt = 0.0);

    // Beat-detector tuning (sensitivity etc.) lives on the detector itself.
    BeatDetector& beatDetector() { return _beat; }

    // Bass-band RMS amplitude of the last analyzed frame (linear, what the
    // detector saw) — diagnostic/calibration aid.
    float lastBassRms() const { return _lastBassRms; }

    static constexpr int kFFT = 1024;            // FFT window; yields 512 bins

private:
    platform::RealFft _fft;

    // Per slot: a ring of the last kRing samples per channel. push() writes,
    // then publishes the total frame count (release); analyze() reads the
    // count (acquire) and copies the newest kFFT samples. The writer would
    // have to lap by kRing - kFFT frames (~150 ms) during a microsecond copy
    // to tear it. A slot whose count did not move since the previous
    // analyze() is stale (its source stopped) and is left out of the sum.
    static constexpr int kRing = 8192;
    struct Slot {
        std::vector<float> ring[kMaxChannels];
        std::atomic<uint64_t> written{0};
        std::atomic<int> channels{2};
        uint64_t seen = 0;
    };
    Slot _slots[kSlots];
    uint64_t _frameIndex = 0;

    std::vector<float> _window;                  // normalized Hann window (kFFT)

    BeatDetector _beat;
    float _lastBassRms = 0.f;
    std::chrono::steady_clock::time_point _lastAnalyze{};
    double _elapsed = 0.0;                       // seconds since first analyze()
    bool _haveClock = false;
};

} // namespace viz
