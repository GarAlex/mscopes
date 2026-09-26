//
// Features.h — audio features beyond AVS's spectrum and waveform:
//
//   - 32 log-spaced bands (30 Hz..16 kHz), each with its own automatic gain
//     and attack/release smoothing, so quiet treble moves as visibly as
//     loud bass; overall level; slow low/mid/high averages
//   - kick, snare and hi-hat hits from band-limited spectral flux, with
//     decaying envelopes, and an overall onset strength
//   - a triggered waveform that holds periodic sound still
//   - every sample since the previous frame
//
// It runs beside the classic analysis and never changes it: VizFrame's
// spectrum, waveform, bass/mid/treble and beat stay exactly what AVS presets
// were written against. Everything here is in addition.
//
// Onsets are found on fixed hops of the audio (256 samples), not per
// display frame, so detection does not depend on the frame rate; the frame
// reports whether a hit happened since the previous one.
//
// Portable C++, no dependencies beyond platform/fft.h.
//
#pragma once
#include "VizFrame.h"
#include "../platform/fft.h"
#include <cstdint>
#include <vector>

namespace viz {

// One band-limited onset detector: peak picking on a spectral-flux signal
// against an adaptive threshold (recent mean + k standard deviations), a
// floor relative to the recent maximum, an absolute floor, and a refractory
// period. A peak is confirmed one hop after it happens.
class OnsetDetector {
public:
    float k = 1.6f;                 // threshold in standard deviations
    float relFloor = 0.2f;          // fraction of the recent maximum flux
    float absFloor = 1e-6f;         // below this it is nothing, whatever the statistics
    double refractory = 0.09;       // seconds between hits

    // Feed this hop's flux; `t` is the time of this hop in seconds. Returns
    // true when the previous hop was a peak above the threshold (and outside
    // the refractory period). The caller decides whether it counts and
    // calls accept(); only accepted hits start a refractory period.
    bool process(float flux, double t, double hopSeconds);
    void accept() { _lastHit = _candidate; }
    void reset();

    float lastFlux() const { return _f1; }
    float threshold() const { return _thr; }
    // How far `f` stands above this region's recent flux, in standard
    // deviations (the statistics of the last process() call).
    float z(float f) const { return (f - _mean) / std::max(_std, 1e-9f); }

private:
    static constexpr int kN = 192;  // ~1 s of hops
    float _hist[kN] = {};
    int _pos = 0, _count = 0;
    float _f1 = 0.f, _f2 = 0.f;     // flux one and two hops ago
    float _runMax = 0.f;
    float _thr = 0.f, _mean = 0.f, _std = 0.f;
    double _lastHit = -1e9, _candidate = -1e9;
};

class AudioFeatures {
public:
    AudioFeatures();
    ~AudioFeatures();

    void setSampleRate(double hz);
    double sampleRate() const { return _sr; }
    void reset();

    // Append `count` new samples per channel (oldest first; chans[c] for
    // c < channels) and fill the feature fields of `out`. `dt` is the time
    // since the previous call (smoothing and envelopes).
    void process(const float* const* chans, int channels, int count, double dt, VizFrame& out);

    // Hop size and window of the onset analysis (tests).
    static constexpr int kHop = 256;
    static constexpr int kOnsetN = 1024;

    // Tuning aid: with debugCandidates set, every drum candidate a detector
    // proposes is recorded with the numbers that decided it; drain them
    // with takeCandidates(). Off by default (no cost).
    struct Candidate {
        char kind;                 // 'k', 's', 'h'
        double t;                  // seconds of audio processed
        float own;                 // its region's flux
        float nk, ns, nh;          // the regions' largest flux over the last four hops
        float spread;              // fraction of its region's bins that rose together
        float zOwn, zS, zH;        // own, snare-band and hat-band flux in standard deviations
        float lowJump;             // low energy against the moment before (kick)
        bool accepted;
    };
    bool debugCandidates = false;
    std::vector<Candidate> takeCandidates() { std::vector<Candidate> c; c.swap(_cands); return c; }

    // Detectors (tests and tuning).
    const OnsetDetector& kickDetector() const { return _kick; }
    const OnsetDetector& snareDetector() const { return _snare; }
    const OnsetDetector& hatDetector() const { return _hat; }

private:
    double _sr = 48000.0;

    // History of what came in: per channel, a mono mix, and a low-passed
    // mono copy for finding zero crossings.
    static constexpr int kHist = 16384;
    std::vector<float> _hist[kMaxChannels];
    std::vector<float> _mono, _lp;
    uint64_t _written = 0;
    float _lp1 = 0.f, _lp2 = 0.f, _lpA = 0.f;

    // Onsets.
    platform::RealFft _fftOnset{kOnsetN};
    std::vector<float> _winOnset, _prevLog;
    bool _havePrevLog = false;
    uint64_t _nextHopEnd = kOnsetN;
    OnsetDetector _kick, _snare, _hat, _full;
    uint64_t _kickAt = 0, _snareAt = 0, _hatAt = 0;     // sample index of the last hit
    bool _kickSeen = false, _snareSeen = false, _hatSeen = false;
    float _onset = 0.f, _fullMax = 0.f;
    float _hk[4] = {}, _hs[4] = {}, _hh[4] = {};   // region fluxes of the last four hops
    int _hpos = 0;
    float _spS[4] = {}, _spH[4] = {};              // spread of the snare and hat bands, same hops
    std::vector<Candidate> _cands;
    static constexpr int kELow = 32;               // ~170 ms of low-band energy
    float _eLow[kELow] = {};
    int _ePos = 0;
    void runHop(uint64_t end, bool& kickHit, bool& snareHit, bool& hatHit, float& onsetPeak);

    // Bands.
    static constexpr int kBandN = 4096;
    platform::RealFft _fftBands{kBandN};
    std::vector<float> _winBands, _scratch;
    float _peakDb[kBands];
    float _bands[kBands] = {};
    float _att[3] = {};
    float _levelPeakDb = -60.f, _level = 0.f;

    // Scope trigger.
    float _prevScope[kWaveformSamples] = {};
    bool _havePrevScope = false;

    float envelope(bool seen, uint64_t at, double tau) const;
};

} // namespace viz
