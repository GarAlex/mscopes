//
// BeatDetector.cpp — see BeatDetector.h.
//
#include "BeatDetector.h"
#include <algorithm>
#include <cmath>

namespace viz {

void BeatDetector::reset()
{
    *this = BeatDetector{};
}

bool BeatDetector::process(float energy, double dt)
{
    energy = std::max(0.f, energy);
    dt = std::max(0.0, std::min(dt, 0.25));       // a stall isn't a 5-second frame
    _clock += dt;

    // Local statistics over the recent window (excluding this sample).
    float mean = 0.f, var = 0.f;
    if (_histCount > 0) {
        for (int i = 0; i < _histCount; ++i) mean += _hist[i];
        mean /= _histCount;
        for (int i = 0; i < _histCount; ++i) {
            float d = _hist[i] - mean;
            var += d * d;
        }
        var /= _histCount;
    }

    // Adaptive threshold (Patin-style), made scale-invariant by using the
    // relative variance var/mean². Bursty material (clear, separated hits)
    // gets a lower ratio; steady, dense material needs a bigger jump so it
    // doesn't trigger on every wobble.
    float rv = mean > 1e-6f ? var / (mean * mean) : 0.f;
    float threshold = std::max(1.2f, std::min(1.6f, 1.6f - 0.5f * rv));
    threshold = 1.f + (threshold - 1.f) / std::max(0.25f, sensitivity);

    bool rising   = energy > _prevEnergy * 1.05f;
    bool loud     = _histCount >= kHistory / 3 && energy > mean * threshold;
    bool audible  = mean > silenceFloor;
    bool rested   = _lastBeat < 0 || (_clock - _lastBeat) >= refractorySeconds;

    bool beat = rising && loud && audible && rested;
    if (beat) noteBeat();

    // Phase advances against the current interval estimate.
    if (_interval > 0.0 && _lastBeat >= 0.0)
        _phase = (float)std::fmod((_clock - _lastBeat) / _interval, 1.0);
    else
        _phase = 0.f;

    _hist[_histPos] = energy;
    _histPos = (_histPos + 1) % kHistory;
    _histCount = std::min(_histCount + 1, kHistory);
    _prevEnergy = energy;
    return beat;
}

void BeatDetector::noteBeat()
{
    if (_lastBeat >= 0.0) {
        double iv = _clock - _lastBeat;
        // Only intervals in a musical range train the tempo (40..250 BPM).
        if (iv >= 0.24 && iv <= 1.5) {
            _intervals[_intervalPos] = iv;
            _intervalPos = (_intervalPos + 1) % kIntervals;
            _intervalCount = std::min(_intervalCount + 1, kIntervals);
        }
    }
    _lastBeat = _clock;
    _phase = 0.f;

    if (_intervalCount >= 3) {
        double sorted[kIntervals];
        std::copy(_intervals, _intervals + _intervalCount, sorted);
        std::sort(sorted, sorted + _intervalCount);
        double median = sorted[_intervalCount / 2];
        // Confidence: how tightly the intervals cluster around the median.
        double dev = 0.0;
        for (int i = 0; i < _intervalCount; ++i) dev += std::fabs(sorted[i] - median);
        dev /= _intervalCount;
        _confidence = (float)std::max(0.0, 1.0 - dev / (median * 0.5));
        _interval = median;
        _bpm = (float)(60.0 / median);
    }
}

} // namespace viz
