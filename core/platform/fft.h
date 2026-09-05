//
// platform/fft.h — the real FFT the Analyzer runs each frame.
//
// One interface, two implementations: Apple's vDSP (fft.mm, used by the
// app) and our own radix-2 (fft.cpp, every other platform and the CLI).
// Both return magnitudes in vDSP's convention (2x the mathematical DFT), so
// the Analyzer's calibration is identical everywhere; beat_selftest checks
// the two agree.
//
#pragma once
#include <vector>

namespace viz { namespace platform {

class RealFft {
public:
    explicit RealFft(int n);            // n = power of two
    ~RealFft();
    RealFft(const RealFft&) = delete;
    RealFft& operator=(const RealFft&) = delete;

    int size() const { return _n; }

    // in: n real samples (already windowed). mags: n/2 magnitudes, bin 0 =
    // DC (with the packed Nyquist term dropped, as the Analyzer always did).
    void magnitudes(const float* in, float* mags);

private:
    int _n;
    void* _impl = nullptr;              // vDSP setup, or our twiddle tables
    std::vector<float> _re, _im, _cos, _sin;
    std::vector<int> _rev;
};

}} // namespace viz::platform
