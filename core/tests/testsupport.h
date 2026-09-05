//
// testsupport.h — shared helpers for the self-tests and tools: deterministic
// synthetic audio, a PNG writer, and a recursive effect count.
//
#pragma once
#include "VizFrame.h"
#include "EffectHost.h"
#include "EffectList.h"
#include "../platform/image.h"
#include <cmath>
#include <functional>
#include <string>

namespace viz { namespace test {

// Deterministic synthetic audio: two tones with a pulsing amplitude, a
// moving spectral peak, a beat every 32 frames. Every render test uses it,
// so PNGs are comparable across runs and backends.
inline void synthAudio(VizFrame& f, int frame)
{
    double t = frame / 60.0;
    f.numSpectrumChannels = f.numWaveformChannels = 2;
    double amp = 0.5 + 0.4 * std::sin(t * 2.2);
    for (int i = 0; i < kWaveformSamples; ++i) {
        double p = (double)i / kWaveformSamples * M_PI * 2;
        float v = (float)(amp * (0.7 * std::sin(p * 6 + t * 4) + 0.3 * std::sin(p * 13 - t * 3)));
        f.waveform[0][i] = f.waveform[1][i] = std::max(-1.f, std::min(1.f, v));
    }
    double peakPos = 0.12 + 0.08 * (0.5 + 0.5 * std::sin(t * 0.9));
    double pulse = 0.6 + 0.4 * std::sin(t * 3.1);
    for (int i = 0; i < kSpectrumBins; ++i) {
        double fr = (double)i / kSpectrumBins;
        double v = pulse * (1.2 * std::exp(-70 * (fr - peakPos) * (fr - peakPos))
                          + 0.5 * std::exp(-40 * (fr - 0.35) * (fr - 0.35)) * (0.5 + 0.5 * std::sin(t * 2))
                          + 0.25 * (1 - fr));
        f.spectrum[0][i] = f.spectrum[1][i] = (float)std::max(0.0, std::min(1.0, v));
    }
    f.computeBands();
    f.beat = (frame % 32) == 0;
    f.frameIndex = frame;
}

inline bool writePng(const Framebuffer& fb, const std::string& path)
{
    return platform::writePng(fb, path);
}

// The whole preset is one implicit root list, so host.count() is 0 or 1;
// walk into lists for a useful number.
inline size_t countEffects(EffectHost& host)
{
    std::function<size_t(Effect*)> rec = [&](Effect* e) -> size_t {
        size_t n = 1;
        if (auto* list = dynamic_cast<EffectListEffect*>(e))
            for (size_t i = 0; i < list->childCount(); ++i) n += rec(list->childAt(i));
        return n;
    };
    size_t n = 0;
    for (size_t i = 0; i < host.count(); ++i) n += rec(host.at(i));
    return n;
}

}} // namespace viz::test
