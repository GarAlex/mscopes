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
#include <cmath>
#include <cstring>

namespace viz {

static constexpr int kSpectrumBins    = 512;   // matches Music's kVisualNumSpectrumEntries
static constexpr int kWaveformSamples = 512;
static constexpr int kMaxChannels     = 2;
static constexpr int kBands           = 32;    // log-spaced analysis bands (VizFrame::bands)
static constexpr int kRecentMax       = 2048;  // samples since the previous frame, at most

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

    // --- Audio features (audio/Features.h) ------------------------------
    // Filled by the system-audio Analyzer beside the classic fields above,
    // which stay exactly what AVS presets were written against. Sources
    // without them (the Music plugin, synthetic test frames) leave
    // hasFeatures false, and effects fall back to the classic fields.
    bool  hasFeatures = false;
    float bands[kBands] = {};      // 30 Hz..16 kHz, log-spaced; each band auto-gained 0..1, attack/release smoothed
    float level = 0.f;             // overall loudness, auto-gained 0..1
    float bassAtt = 0.f;           // slow averages of the bands: lows (< 250 Hz),
    float midAtt = 0.f;            //   mids (250 Hz..4 kHz)
    float trebleAtt = 0.f;         //   and highs (> 4 kHz), 0..1
    float onset = 0.f;             // onset strength (spectral flux), 0..1, decaying
    float kick = 0.f;              // drum envelopes: 1 at a hit, decaying
    float snare = 0.f;
    float hat = 0.f;
    bool  kickHit = false;         // a hit since the previous frame
    bool  snareHit = false;
    bool  hatHit = false;
    // Triggered waveform: the same length and scale as `waveform`, started at
    // a rising zero crossing chosen to line up with the previous frame, so
    // periodic sound holds still instead of sliding.
    float scope[kMaxChannels][kWaveformSamples] = {};
    // Every sample since the previous frame, oldest first (at 60 fps and
    // 48 kHz, 800 of them; `waveform` only shows the newest 512).
    int   recentCount = 0;
    float recent[kMaxChannels][kRecentMax] = {};

    // The bands as a curve over x = 0..1 (low to high), interpolated.
    float bandAt(float x) const {
        float p = std::fmax(0.f, std::fmin(1.f, x)) * (kBands - 1);
        int i = (int)p;
        if (i >= kBands - 1) return bands[kBands - 1];
        return bands[i] + (bands[i + 1] - bands[i]) * (p - (float)i);
    }

    // Band b covers bandEdgeHz(b) .. bandEdgeHz(b + 1).
    static float bandEdgeHz(int i) {
        float t = (float)i / (float)kBands;
        return 30.f * std::pow(16000.f / 30.f, t);
    }

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
