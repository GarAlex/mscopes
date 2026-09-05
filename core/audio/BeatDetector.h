//
// BeatDetector.h — bass-energy onset detection + BPM/phase estimation.
//
// Pure C++, no deps, so every audio source (system tap Analyzer, the Music
// plugin's pulse feed) can run the same detector on the same signal: the
// frame's bass level (0..1). Until this existed the live app never set
// VizFrame::beat at all — only the test harnesses faked one every 32 frames,
// so every on-beat effect and every preset's beat script was dead in
// production.
//
// Algorithm (the classic energy-flux detector AVS/Milkdrop-era code used):
//   - keep ~0.75 s of recent bass energy; local mean + variance
//   - a beat is an energy sample that both RISES from the previous sample
//     and exceeds mean * threshold, where the threshold adapts with variance
//     (steady, loud material needs a bigger jump than sparse material)
//   - refractory period so one kick isn't reported twice (caps at ~400 BPM)
//   - silence gate: near-silent input never beats on the noise floor
//   - BPM = 60 / median of the last 8 inter-beat intervals; beatPhase = time
//     since the last beat as a 0..1 fraction of that interval (wraps)
//
#pragma once
#include <cstddef>

namespace viz {

class BeatDetector {
public:
    // Feed one analysis frame. `energy` is the bass-band level in any LINEAR
    // amplitude/energy unit (the detector is ratio-based, so absolute scale
    // only matters for `silenceFloor`); `dt` is seconds since the previous
    // call. Returns true on a detected onset.
    bool process(float energy, double dt);

    void reset();

    float bpm() const { return _bpm; }             // 0 until enough beats seen
    float beatPhase() const { return _phase; }     // 0..1, 0 at each beat
    float confidence() const { return _confidence; } // 0..1, interval regularity

    // Tuning. sensitivity 1.0 = default; >1 fires more readily.
    float sensitivity = 1.0f;
    double refractorySeconds = 0.15;               // 400 BPM ceiling
    // Mean energy below this → no beats. Default is calibrated for the
    // Analyzer's bass RMS amplitude (≈ -50 dBFS); log-domain feeds (the Music
    // plugin's 0..1 spectrum) should raise it.
    float silenceFloor = 0.003f;

private:
    static constexpr int kHistory = 45;            // ~0.75 s at 60 fps
    float _hist[kHistory] = {};
    int _histPos = 0;
    int _histCount = 0;
    float _prevEnergy = 0.f;
    double _clock = 0.0;
    double _lastBeat = -1.0;

    static constexpr int kIntervals = 8;
    double _intervals[kIntervals] = {};
    int _intervalPos = 0;
    int _intervalCount = 0;
    double _interval = 0.0;                        // current estimate (s)

    float _bpm = 0.f, _phase = 0.f, _confidence = 0.f;

    void noteBeat();
};

} // namespace viz
