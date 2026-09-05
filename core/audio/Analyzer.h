//
// Analyzer.h — turns raw interleaved float PCM into a shared viz::VizFrame.
//
// Standalone-only: the Music plugin gets a spectrum from Music already, but for
// arbitrary system audio we must run our own FFT. Thread model: the audio
// callback calls push() (real-time thread); a timer on the main thread calls
// analyze() to produce a frame. A mutex guards the shared sample ring.
// Portable C++: the FFT comes from platform/fft.h (vDSP on Apple, our own
// radix-2 elsewhere).
//
#pragma once
#include "VizFrame.h"
#include "BeatDetector.h"
#include "../platform/fft.h"
#include <chrono>
#include <mutex>
#include <vector>
#include <cstdint>

namespace viz {

class Analyzer {
public:
    Analyzer();
    ~Analyzer();

    // Called from the audio thread. Interleaved float frames, `channels` wide.
    void push(const float* interleaved, int numFrames, int channels);

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

    std::mutex _mutex;
    std::vector<float> _ring[kMaxChannels];      // last kFFT samples per channel
    int _writePos = 0;
    int _channels = 2;
    uint64_t _frameIndex = 0;

    std::vector<float> _window;                  // normalized Hann window (kFFT)

    BeatDetector _beat;
    float _lastBassRms = 0.f;
    std::chrono::steady_clock::time_point _lastAnalyze{};
    double _elapsed = 0.0;                       // seconds since first analyze()
    bool _haveClock = false;
};

} // namespace viz
