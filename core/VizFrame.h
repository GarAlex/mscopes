//
// VizFrame.h — the shared audio-analysis frame that every front-end produces
// and the renderer/effect host consume.
//
// This is the portable seam between audio sources (Music plugin pulse feed, the
// standalone CoreAudio system tap, future backends) and everything downstream.
// Pure C++ — no UI framework, no host SDK dependency.
//
#pragma once
#include <cstdint>
#include <cstring>

namespace viz {

static constexpr int kSpectrumBins    = 512;   // matches Music's kVisualNumSpectrumEntries
static constexpr int kWaveformSamples = 512;
static constexpr int kMaxChannels     = 2;

struct VizFrame {
    int   numSpectrumChannels = 0;
    int   numWaveformChannels = 0;

    float spectrum[kMaxChannels][kSpectrumBins]   = {};  // normalized 0..1
    float waveform[kMaxChannels][kWaveformSamples] = {}; // normalized -1..1

    // Pre-reduced bands (0..1), convenient for audio-reactive effects.
    float bass   = 0.f;
    float mid    = 0.f;
    float treble = 0.f;

    bool     beat       = false; // onset detected this frame (BeatDetector)
    float    bpm        = 0.f;   // tempo estimate, 0 until locked
    float    beatPhase  = 0.f;   // 0..1 progress through the current beat
    double   time       = 0.0;   // seconds since source start
    uint64_t frameIndex = 0;

    void clear() { *this = VizFrame{}; }

    // Peak spectrum value across channel 0 (diagnostic).
    float peakSpectrum() const {
        float p = 0.f;
        for (int i = 0; i < kSpectrumBins; ++i) if (spectrum[0][i] > p) p = spectrum[0][i];
        return p;
    }

    // Fill bass/mid/treble from channel-0 spectrum (call after populating spectrum).
    void computeBands() {
        auto avg = [this](int lo, int hi) {
            float s = 0.f; int n = 0;
            for (int i = lo; i < hi && i < kSpectrumBins; ++i) { s += spectrum[0][i]; ++n; }
            return n ? s / n : 0.f;
        };
        bass   = avg(0,   kSpectrumBins / 16);          // ~lowest 1/16
        mid    = avg(kSpectrumBins / 16, kSpectrumBins / 4);
        treble = avg(kSpectrumBins / 4,  kSpectrumBins);
    }
};

} // namespace viz
