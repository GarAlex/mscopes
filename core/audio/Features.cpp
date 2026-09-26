//
// Features.cpp — see Features.h.
//
#include "Features.h"
#include <algorithm>
#include <cmath>

namespace viz {

namespace {

inline float smoothstep(float a, float b, float x)
{
    float t = std::clamp((x - a) / (b - a), 0.f, 1.f);
    return t * t * (3.f - 2.f * t);
}

// One-pole follower with separate attack and release time constants.
inline float follow(float y, float target, double dt, double attack, double release)
{
    double tau = target > y ? attack : release;
    return y + (target - y) * (float)(1.0 - std::exp(-dt / tau));
}

void hann(std::vector<float>& w, int n)
{
    w.resize((size_t)n);
    for (int i = 0; i < n; ++i)
        w[(size_t)i] = 0.5f * (1.f - std::cos(2.f * (float)M_PI * (float)i / (float)n));
}

} // namespace

// ---------------------------------------------------------------------------
//  OnsetDetector
// ---------------------------------------------------------------------------
void OnsetDetector::reset()
{
    std::fill(_hist, _hist + kN, 0.f);
    _pos = _count = 0;
    _f1 = _f2 = _runMax = _thr = 0.f;
    _lastHit = _candidate = -1e9;
}

bool OnsetDetector::process(float f, double t, double hopSeconds)
{
    // Statistics of the recent flux, up to and including the candidate
    // (the previous hop); the current hop only confirms it was a peak.
    float mean = 0.f, var = 0.f;
    if (_count > 0) {
        for (int i = 0; i < _count; ++i) mean += _hist[i];
        mean /= (float)_count;
        for (int i = 0; i < _count; ++i) { float d = _hist[i] - mean; var += d * d; }
        var /= (float)_count;
    }
    _mean = mean;
    _std = std::sqrt(var);
    _thr = mean + k * _std;
    _runMax = std::max(_f1, _runMax * (float)std::exp(-hopSeconds / 2.0));
    const float floorV = std::max(absFloor, relFloor * _runMax);
    const double tPeak = t - hopSeconds;

    const bool hit = _count >= kN / 4
                  && _f1 > _f2 && _f1 >= f
                  && _f1 > _thr && _f1 > floorV
                  && tPeak - _lastHit >= refractory;
    if (hit) _candidate = tPeak;

    _hist[_pos] = f;
    _pos = (_pos + 1) % kN;
    _count = std::min(_count + 1, kN);
    _f2 = _f1;
    _f1 = f;
    return hit;
}

// ---------------------------------------------------------------------------
//  AudioFeatures
// ---------------------------------------------------------------------------
AudioFeatures::AudioFeatures()
{
    for (auto& h : _hist) h.assign(kHist, 0.f);
    _mono.assign(kHist, 0.f);
    _lp.assign(kHist, 0.f);
    hann(_winOnset, kOnsetN);
    hann(_winBands, kBandN);
    _prevLog.assign(kOnsetN / 2, 0.f);

    // Kick: the low band is sparse and hits are strong; hats are dense and
    // short, so a shorter refractory period and a slightly lower threshold.
    _kick.refractory = 0.10;
    _snare.refractory = 0.10;
    _hat.refractory = 0.05;
    _hat.k = 1.4f;
    setSampleRate(48000.0);
    reset();
}

AudioFeatures::~AudioFeatures() = default;

void AudioFeatures::setSampleRate(double hz)
{
    if (!(hz > 1000.0)) hz = 48000.0;
    _sr = hz;
    // Two cascaded one-pole low-passes at ~350 Hz: the trigger looks for
    // zero crossings of the fundamental, not of every harmonic.
    _lpA = 1.f - (float)std::exp(-2.0 * M_PI * 350.0 / _sr);
}

void AudioFeatures::reset()
{
    for (auto& h : _hist) std::fill(h.begin(), h.end(), 0.f);
    std::fill(_mono.begin(), _mono.end(), 0.f);
    std::fill(_lp.begin(), _lp.end(), 0.f);
    _written = 0;
    _lp1 = _lp2 = 0.f;
    std::fill(_prevLog.begin(), _prevLog.end(), 0.f);
    _havePrevLog = false;
    _nextHopEnd = kOnsetN;
    _kick.reset(); _snare.reset(); _hat.reset();
    _kickAt = _snareAt = _hatAt = 0;
    _kickSeen = _snareSeen = _hatSeen = false;
    _onset = 0.f; _fullMax = 0.f;
    std::fill(_hk, _hk + 4, 0.f); std::fill(_hs, _hs + 4, 0.f); std::fill(_hh, _hh + 4, 0.f); _hpos = 0;
    std::fill(_eLow, _eLow + kELow, 0.f); _ePos = 0;
    for (int b = 0; b < kBands; ++b) { _peakDb[b] = -60.f; _bands[b] = 0.f; }
    _att[0] = _att[1] = _att[2] = 0.f;
    _levelPeakDb = -60.f; _level = 0.f;
    std::fill(_prevScope, _prevScope + kWaveformSamples, 0.f);
    _havePrevScope = false;
}

float AudioFeatures::envelope(bool seen, uint64_t at, double tau) const
{
    if (!seen || at > _written) return 0.f;
    double age = (double)(_written - at) / _sr;
    return (float)std::exp(-age / tau);
}

void AudioFeatures::runHop(uint64_t end, bool& kickHit, bool& snareHit, bool& hatHit, float& onsetPeak)
{
    constexpr int N = kOnsetN, H = N / 2;
    float buf[N], mag[H], lg[H];
    for (int i = 0; i < N; ++i)
        buf[i] = _mono[(size_t)((end - (uint64_t)N + (uint64_t)i) % kHist)] * _winOnset[(size_t)i];
    _fftOnset.magnitudes(buf, mag);

    // Linear magnitudes (a full-scale sine's peak bin is 1). Flux is the
    // mean rise per bin in a region; every threshold below is relative
    // (the region's own recent statistics, or another region's flux in the
    // same hop), so a hit reads the same loud or quiet.
    const float scale = 2.f / (float)N;
    double s2 = 0.0;
    for (int k = 0; k < H; ++k) { lg[k] = mag[k] * scale; s2 += (double)lg[k] * lg[k]; }
    if (!_havePrevLog) {
        std::copy(lg, lg + H, _prevLog.begin());
        _havePrevLog = true;
        return;
    }

    const float binHz = (float)(_sr / N);
    auto flux = [&](float f0, float f1) {
        int k0 = std::max(1, (int)std::ceil(f0 / binHz));
        int k1 = std::min(H, (int)std::ceil(f1 / binHz));
        if (k1 <= k0) k1 = std::min(H, k0 + 1);
        float s = 0.f;
        for (int k = k0; k < k1; ++k) s += std::max(0.f, lg[k] - _prevLog[(size_t)k]);
        return k1 > k0 ? s / (float)(k1 - k0) : 0.f;
    };
    // Kick: the thump below 120 Hz (a snare's body leaks into anything
    // higher). Snare: the rattle of the wires, 1.5-5 kHz. Hi-hat: the top.
    const float fk = flux(35.f, 120.f);
    float eLow = 0.f;                                  // low-band energy this hop
    for (int k = std::max(1, (int)std::ceil(35.f / binHz)); k < std::min(H, (int)std::ceil(120.f / binHz)); ++k)
        eLow += lg[k] * lg[k];
    const float fs = flux(1500.f, 5000.f);
    const float fh = flux(7000.f, 16000.f);
    // Spread: the share of a band's bins that rose together. A drum's
    // noise lifts nearly all of them; a sung or played note lifts a few
    // harmonics.
    auto spread = [&](float f0, float f1) {
        int k0 = std::max(1, (int)std::ceil(f0 / binHz));
        int k1 = std::min(H, (int)std::ceil(f1 / binHz));
        if (k1 <= k0) return 0.f;
        float mx = 0.f;
        for (int k = k0; k < k1; ++k) mx = std::max(mx, lg[k] - _prevLog[(size_t)k]);
        if (mx <= 0.f) return 0.f;
        int c = 0;
        for (int k = k0; k < k1; ++k) c += (lg[k] - _prevLog[(size_t)k]) > 0.25f * mx;
        return (float)c / (float)(k1 - k0);
    };
    const float spreadS = spread(1500.f, 5000.f), spreadH = spread(7000.f, 16000.f);
    const float ff = flux(35.f, 16000.f);
    std::copy(lg, lg + H, _prevLog.begin());

    const double hopSec = (double)kHop / _sr;
    const double t = (double)end / _sr;
    const uint64_t peakAt = end - (uint64_t)kHop;      // the hop the peak was in
    // Nothing below about -70 dBFS counts (hiss, fades, silence).
    const bool audible = std::sqrt(s2) > 3e-4;
    // Each region's detector proposes; the other regions decide whether it
    // is really that drum, by how the rise is spread over the spectrum.
    const bool kc = _kick.process(fk, t, hopSec);
    const bool sc = _snare.process(fs, t, hopSec);
    const bool hc = _hat.process(fh, t, hopSec);
    // A detector confirms its peak one hop late, and the regions of one
    // hit peak a hop or two apart (the low end builds up slower), so each
    // candidate's own flux is the previous hop's and the other regions are
    // taken at their largest over the last four hops.
    _hk[_hpos] = fk; _hs[_hpos] = fs; _hh[_hpos] = fh;
    _spS[_hpos] = spreadS; _spH[_hpos] = spreadH;
    const int prev = (_hpos + 3) % 4;
    _hpos = (_hpos + 1) % 4;
    const float ok = _hk[prev], os = _hs[prev], oh = _hh[prev];
    const float nk = std::max({_hk[0], _hk[1], _hk[2], _hk[3]});
    const float ns = std::max({_hs[0], _hs[1], _hs[2], _hs[3]});
    const float nh = std::max({_hh[0], _hh[1], _hh[2], _hh[3]});
    // A kick is a jump in low energy over the moment before it; two
    // sustained low notes beating against each other rise and fall about
    // their average and never jump like that.
    _eLow[_ePos] = eLow;
    _ePos = (_ePos + 1) % kELow;
    float before = 0.f;
    for (int i = 3; i < kELow; ++i) before += _eLow[(size_t)((_ePos - 1 - i + 2 * kELow) % kELow)];
    before /= (float)(kELow - 3);
    const float nowE = std::max(_eLow[(size_t)((_ePos - 1 + kELow) % kELow)], _eLow[(size_t)((_ePos - 2 + kELow) % kELow)]);
    const float jump = nowE / std::max(before, 1e-12f);
    // Tuned on synthetic drums (every hit, no spill) and checked on real
    // music (hit rates and intervals against the tempo):
    //   kick   low energy jumps, the low rise stands out from its own recent
    //          flux, and it dwarfs the wire band (a snare's low spill is
    //          about 17x the wire band's rise; a kick's, hundreds)
    //   snare  the wire band rises broadly (a note lifts a few harmonics:
    //          on real music half the "snares" were notes), more than a
    //          kick's click, and not just the edge of a hi-hat
    //   hat    the top rises well above the wire band, in flux or in
    //          standard deviations (in a full mix the wire band is always
    //          busy, so the plain ratio misses most real hats)
    const float zK = _kick.z(ok), zS = _snare.z(os), zH = _hat.z(oh);
    const float spS = _spS[prev];
    const bool kOk = audible && kc && jump >= 2.5f && zK >= 3.f && ok >= 40.f * ns;
    const bool sOk = audible && sc && spS >= 0.3f && os >= 0.005f * nk && os >= 0.25f * nh;
    const bool hOk = audible && hc && (oh >= 3.f * ns || zH - zS >= 2.f);
    if (kOk) { _kick.accept();  kickHit = true;  _kickAt = peakAt;  _kickSeen = true; }
    if (sOk) { _snare.accept(); snareHit = true; _snareAt = peakAt; _snareSeen = true; }
    if (hOk) { _hat.accept();   hatHit = true;   _hatAt = peakAt;   _hatSeen = true; }
    if (debugCandidates && audible) {
        const double tp = t - hopSec;
        if (kc) _cands.push_back({'k', tp, ok, nk, ns, nh, 0.f, zK, zS, zH, jump, kOk});
        if (sc) _cands.push_back({'s', tp, os, nk, ns, nh, spS, zS, zS, zH, jump, sOk});
        if (hc) _cands.push_back({'h', tp, oh, nk, ns, nh, _spH[prev], zH, zS, zH, jump, hOk});
    }

    // Overall onset strength: full-band flux against its recent maximum.
    _fullMax = std::max(ff, _fullMax * (float)std::exp(-hopSec / 3.0));
    if (audible && _fullMax > 1e-6f) onsetPeak = std::max(onsetPeak, std::min(1.f, ff / _fullMax));
}

void AudioFeatures::process(const float* const* chans, int channels, int count, double dt, VizFrame& out)
{
    if (!(dt > 0.0)) dt = 1.0 / 60.0;
    dt = std::min(dt, 0.25);
    channels = std::clamp(channels, 1, kMaxChannels);
    count = std::max(0, count);

    // ---- append --------------------------------------------------------
    for (int i = 0; i < count; ++i) {
        const size_t p = (size_t)((_written + (uint64_t)i) % kHist);
        float m = 0.f;
        for (int c = 0; c < kMaxChannels; ++c) {
            float v = chans[std::min(c, channels - 1)][i];
            _hist[c][p] = v;
            m += v;
        }
        m *= 1.f / (float)kMaxChannels;
        _mono[p] = m;
        _lp1 += _lpA * (m - _lp1);
        _lp2 += _lpA * (_lp1 - _lp2);
        _lp[p] = _lp2;
    }
    _written += (uint64_t)count;

    // ---- every sample since the previous frame ---------------------------
    const int rc = std::min(count, kRecentMax);
    out.recentCount = rc;
    for (int c = 0; c < kMaxChannels; ++c) {
        const float* src = chans[std::min(c, channels - 1)] + (count - rc);
        std::copy(src, src + rc, out.recent[c]);
    }

    // ---- onsets, on fixed hops -------------------------------------------
    bool kh = false, sh = false, hh = false;
    float onsetPeak = 0.f;
    const uint64_t oldest = _written > (uint64_t)kHist ? _written - (uint64_t)kHist : 0;
    if (_nextHopEnd < oldest + (uint64_t)kOnsetN) {       // a stall longer than the history
        _nextHopEnd = oldest + (uint64_t)kOnsetN;
        _havePrevLog = false;
    }
    while (_nextHopEnd <= _written) {
        runHop(_nextHopEnd, kh, sh, hh, onsetPeak);
        _nextHopEnd += (uint64_t)kHop;
    }
    out.kickHit = kh; out.snareHit = sh; out.hatHit = hh;
    out.kick  = envelope(_kickSeen,  _kickAt,  0.14);
    out.snare = envelope(_snareSeen, _snareAt, 0.12);
    out.hat   = envelope(_hatSeen,   _hatAt,   0.06);
    _onset = std::max(onsetPeak, _onset * (float)std::exp(-dt / 0.12));
    out.onset = _onset;

    // ---- log bands with per-band automatic gain ---------------------------
    {
        constexpr int N = kBandN, H = N / 2;
        std::vector<float>& buf = _scratch;
        buf.resize((size_t)N + (size_t)H);
        float* x = buf.data();
        float* mag = x + N;
        const uint64_t avail = std::min<uint64_t>(_written, (uint64_t)N);
        for (int i = 0; i < N; ++i) {
            const bool have = (uint64_t)i >= (uint64_t)N - avail;
            const uint64_t idx = _written - (uint64_t)N + (uint64_t)i;
            x[i] = have ? _mono[(size_t)(idx % kHist)] * _winBands[(size_t)i] : 0.f;
        }
        _fftBands.magnitudes(x, mag);
        const float scale = 2.f / (float)N;
        for (int k = 0; k < H; ++k) { float m = mag[k] * scale; mag[k] = m * m; }   // power

        const float binHz = (float)(_sr / N);
        float db[kBands];
        float maxPeak = -200.f;
        for (int b = 0; b < kBands; ++b) {
            const float x0 = VizFrame::bandEdgeHz(b) / binHz, x1 = VizFrame::bandEdgeHz(b + 1) / binHz;
            float pw = 0.f;
            if (x1 - x0 >= 2.f) {                                  // wide: sum the bins inside
                for (int k = std::max(1, (int)std::ceil(x0)); k < (int)std::ceil(x1) && k < H; ++k) pw += mag[k];
            } else {                                               // narrower than two bins: density x width
                const float xc = 0.5f * (x0 + x1);
                const int k0 = std::clamp((int)xc, 0, H - 2);
                const float fr = std::clamp(xc - (float)k0, 0.f, 1.f);
                pw = (mag[k0] * (1.f - fr) + mag[k0 + 1] * fr) * (x1 - x0);
            }
            db[b] = 10.f * std::log10(pw + 1e-12f);
            _peakDb[b] = std::max(db[b], _peakDb[b] - (float)(3.0 * dt));   // forgets 3 dB a second
            maxPeak = std::max(maxPeak, _peakDb[b]);
        }
        // Each band against its own recent peak, over a 30 dB range. A band
        // far below the loudest one is not lifted all the way (that would
        // turn empty bands into noise), and silence stays dark.
        constexpr float kRange = 30.f;
        float sum[3] = {}; int n[3] = {};
        for (int b = 0; b < kBands; ++b) {
            const float ceilDb = std::max({_peakDb[b], maxPeak - 24.f, -60.f});
            float v = std::clamp((db[b] - (ceilDb - kRange)) / kRange, 0.f, 1.f);
            v *= smoothstep(-90.f, -72.f, db[b]);
            _bands[b] = follow(_bands[b], v, dt, 0.012, 0.16);
            out.bands[b] = _bands[b];
            const float fc = std::sqrt(VizFrame::bandEdgeHz(b) * VizFrame::bandEdgeHz(b + 1));
            const int g = fc < 250.f ? 0 : fc < 4000.f ? 1 : 2;
            sum[g] += _bands[b]; n[g]++;
        }
        for (int g = 0; g < 3; ++g)
            _att[g] = follow(_att[g], n[g] ? sum[g] / (float)n[g] : 0.f, dt, 0.10, 0.50);
        out.bassAtt = _att[0]; out.midAtt = _att[1]; out.trebleAtt = _att[2];
    }

    // ---- overall level ------------------------------------------------------
    {
        const int n = (int)std::min<uint64_t>(_written, 1024);
        double s2 = 0.0;
        for (int i = 0; i < n; ++i) {
            float v = _mono[(size_t)((_written - (uint64_t)n + (uint64_t)i) % kHist)];
            s2 += (double)v * v;
        }
        const float db = 20.f * std::log10((float)std::sqrt(n ? s2 / n : 0.0) + 1e-9f);
        _levelPeakDb = std::max(db, _levelPeakDb - (float)(2.0 * dt));
        const float ceilDb = std::max(_levelPeakDb, -45.f);
        float v = std::clamp((db - (ceilDb - 24.f)) / 24.f, 0.f, 1.f) * smoothstep(-80.f, -65.f, db);
        _level = follow(_level, v, dt, 0.02, 0.25);
        out.level = _level;
    }

    // ---- triggered waveform -----------------------------------------------
    {
        constexpr int W = kWaveformSamples;
        constexpr int kSearch = 1536, kMaxCand = 48;
        const int64_t written = (int64_t)_written;
        const int64_t sHi = written - W;                            // the newest window
        const int64_t sLo = std::max<int64_t>(std::max<int64_t>(1, sHi - kSearch),
                                              written - (int64_t)kHist + 1);
        int64_t start = std::max<int64_t>(0, sHi);
        if (sHi > sLo) {
            // Rising zero crossings of the low-passed signal, newest first.
            int64_t cand[kMaxCand];
            int nc = 0;
            for (int64_t s = sHi; s > sLo && nc < kMaxCand; --s)
                if (_lp[(size_t)((s - 1) % kHist)] < 0.f && _lp[(size_t)(s % kHist)] >= 0.f)
                    cand[nc++] = s;
            float prevE = 0.f;
            if (_havePrevScope)
                for (int i = 0; i < W; i += 2) prevE += _prevScope[i] * _prevScope[i];
            if (nc > 0) {
                start = cand[0];
                if (prevE > 1e-6f) {
                    // The crossing whose window looks most like the last
                    // frame's, with a slight preference for newer ones.
                    float best = -2.f;
                    for (int j = 0; j < nc; ++j) {
                        float dot = 0.f, e = 0.f;
                        for (int i = 0; i < W; i += 2) {
                            float v = _mono[(size_t)((cand[j] + i) % kHist)];
                            dot += v * _prevScope[i];
                            e += v * v;
                        }
                        float score = dot / std::sqrt(e * prevE + 1e-12f)
                                    + 0.05f * (float)(cand[j] - sLo) / (float)(sHi - sLo);
                        if (score > best) { best = score; start = cand[j]; }
                    }
                }
            }
        }
        for (int i = 0; i < W; ++i) {
            const int64_t idx = start + i;
            const bool have = idx >= 0 && idx < written && idx > written - (int64_t)kHist;
            const size_t p = (size_t)(have ? idx % kHist : 0);
            for (int c = 0; c < kMaxChannels; ++c)
                out.scope[c][i] = have ? std::clamp(_hist[c][p], -1.f, 1.f) : 0.f;
            _prevScope[i] = have ? _mono[p] : 0.f;
        }
        _havePrevScope = true;
    }

    out.hasFeatures = true;
}

} // namespace viz
