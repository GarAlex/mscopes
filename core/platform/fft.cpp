//
// platform/fft.cpp — portable radix-2 real FFT (see fft.h). Not the fastest
// possible, but a 1024-point transform twice a frame is far below the
// noise floor of anything else the engine does.
//
#include "fft.h"
#include <cmath>

namespace viz { namespace platform {

RealFft::RealFft(int n) : _n(n)
{
    _re.assign(n, 0.f); _im.assign(n, 0.f);
    _cos.resize(n / 2); _sin.resize(n / 2);
    for (int k = 0; k < n / 2; ++k) {
        double a = -2.0 * M_PI * k / n;
        _cos[k] = (float)std::cos(a);
        _sin[k] = (float)std::sin(a);
    }
    _rev.resize(n);
    int bits = 0;
    while ((1 << bits) < n) ++bits;
    for (int i = 0; i < n; ++i) {
        int r = 0;
        for (int b = 0; b < bits; ++b) if (i & (1 << b)) r |= 1 << (bits - 1 - b);
        _rev[i] = r;
    }
}

RealFft::~RealFft() {}

void RealFft::magnitudes(const float* in, float* mags)
{
    const int n = _n;
    for (int i = 0; i < n; ++i) { _re[_rev[i]] = in[i]; _im[_rev[i]] = 0.f; }
    for (int len = 2; len <= n; len <<= 1) {
        const int half = len >> 1, step = n / len;
        for (int i = 0; i < n; i += len) {
            for (int j = 0; j < half; ++j) {
                const float c = _cos[j * step], s = _sin[j * step];
                const int a = i + j, b = a + half;
                const float tr = _re[b] * c - _im[b] * s;
                const float ti = _re[b] * s + _im[b] * c;
                _re[b] = _re[a] - tr; _im[b] = _im[a] - ti;
                _re[a] += tr;         _im[a] += ti;
            }
        }
    }
    // vDSP's real FFT packs the Nyquist term into bin 0's imaginary slot and
    // scales by 2; the Analyzer zeroes that slot, so bin 0 is 2*|DC|.
    for (int k = 0; k < n / 2; ++k)
        mags[k] = 2.f * std::sqrt(_re[k] * _re[k] + (k ? _im[k] * _im[k] : 0.f));
}

}} // namespace viz::platform
