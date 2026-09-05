//
// Analyzer.cpp — see Analyzer.h.
//
#include "Analyzer.h"
#include <algorithm>
#include <cmath>

namespace viz {

Analyzer::Analyzer() : _fft(kFFT)
{
    for (int c = 0; c < kMaxChannels; ++c)
        _ring[c].assign(kRing, 0.f);

    // Hann window in vDSP's "normalized" flavor (unit RMS: the plain window
    // scaled by sqrt(8/3)), so calibration matches the original Apple build.
    _window.resize(kFFT);
    const float norm = std::sqrt(8.f / 3.f);
    for (int i = 0; i < kFFT; ++i)
        _window[i] = norm * 0.5f * (1.f - std::cos(2.f * (float)M_PI * (float)i / (float)kFFT));
}

Analyzer::~Analyzer() = default;

void Analyzer::push(const float* interleaved, int numFrames, int channels)
{
    if (!interleaved || numFrames <= 0 || channels <= 0) return;
    _channels.store(std::min(channels, kMaxChannels), std::memory_order_relaxed);
    uint64_t w = _written.load(std::memory_order_relaxed);
    for (int n = 0; n < numFrames; ++n) {
        int pos = (int)((w + (uint64_t)n) % kRing);
        for (int c = 0; c < kMaxChannels; ++c) {
            int src = (c < channels) ? c : (channels - 1);   // dup last ch if mono
            _ring[c][pos] = interleaved[n * channels + src];
        }
    }
    _written.store(w + (uint64_t)numFrames, std::memory_order_release);
}

void Analyzer::analyze(VizFrame& out, double dtOverride)
{
    // Snapshot the newest kFFT samples (oldest first), lock-free.
    float chan[kMaxChannels][kFFT];
    int channels = _channels.load(std::memory_order_relaxed);
    {
        uint64_t w = _written.load(std::memory_order_acquire);
        uint64_t start = w >= (uint64_t)kFFT ? w - (uint64_t)kFFT : 0;
        for (int c = 0; c < kMaxChannels; ++c)
            for (int i = 0; i < kFFT; ++i) {
                uint64_t idx = start + (uint64_t)i;
                chan[c][i] = idx < w ? _ring[c][idx % kRing] : 0.f;
            }
    }

    // dt between analyses feeds the beat detector's tempo clock: wall clock
    // by default, or the caller's own hop for offline use.
    double dt;
    if (dtOverride > 0.0) {
        dt = _haveClock ? dtOverride : 0.0;
        _haveClock = true;
        _elapsed += dt;
    } else {
        const auto now = std::chrono::steady_clock::now();
        dt = _haveClock ? std::chrono::duration<double>(now - _lastAnalyze).count() : 0.0;
        _haveClock = true;
        _lastAnalyze = now;
        _elapsed += dt;
    }
    if (dt <= 0.0) dt = 1.0 / 60.0;

    out.numSpectrumChannels = channels;
    out.numWaveformChannels = channels;
    out.time = _elapsed;
    out.frameIndex = _frameIndex++;

    const int half = kFFT / 2;                  // 512 bins
    float bassRms = 0.f;
    float windowed[kFFT], mags[kFFT / 2];

    for (int c = 0; c < kMaxChannels; ++c) {
        // --- waveform: last kWaveformSamples samples (already -1..1) ---
        for (int i = 0; i < kWaveformSamples; ++i) {
            int idx = kFFT - kWaveformSamples + i;   // most-recent tail
            float v = chan[c][idx];
            out.waveform[c][i] = std::max(-1.f, std::min(1.f, v));
        }

        // --- spectrum: windowed real FFT magnitude ---
        for (int i = 0; i < kFFT; ++i) windowed[i] = chan[c][i] * _window[i];
        _fft.magnitudes(windowed, mags);

        // Normalize to 0..1 with a log curve so quiet detail is visible.
        const float scale = 1.0f / (float)kFFT;
        const float boost = 120.0f;
        const float denom = std::log10(1.0f + boost);
        for (int i = 0; i < kSpectrumBins; ++i) {
            float m = (i < half) ? mags[i] * scale : 0.f;
            float level = std::log10(1.0f + boost * m) / denom;
            out.spectrum[c][i] = std::max(0.f, std::min(1.f, level));
        }

        // Beat detection wants a LINEAR bass level: the log curve above
        // squashes a doubling of kick energy into a ~1.2x ratio, which the
        // onset threshold could not separate from ordinary wobble. Use the
        // RMS of the raw bass-band magnitudes on the mixed/first channel.
        if (c == 0) {
            const int bassBins = kSpectrumBins / 16;      // same band as computeBands()
            float sum = 0.f;
            for (int i = 1; i < bassBins; ++i) {          // skip DC
                float m = mags[i] * scale;
                sum += m * m;
            }
            bassRms = std::sqrt(sum / (bassBins - 1));
        }
    }

    out.computeBands();

    _lastBassRms = bassRms;
    out.beat      = _beat.process(bassRms, dt);
    out.bpm       = _beat.bpm();
    out.beatPhase = _beat.beatPhase();
}

} // namespace viz
