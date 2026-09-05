//
// platform/fft.mm — the Apple implementation of fft.h on vDSP. Same
// contract as fft.cpp; a build uses one or the other.
//
#import <Accelerate/Accelerate.h>
#include "fft.h"
#include <cmath>

namespace viz { namespace platform {

RealFft::RealFft(int n) : _n(n)
{
    int log2n = 0;
    while ((1 << log2n) < n) ++log2n;
    _impl = (void*)vDSP_create_fftsetup((vDSP_Length)log2n, FFT_RADIX2);
    _re.assign(n / 2, 0.f); _im.assign(n / 2, 0.f);
}

RealFft::~RealFft()
{
    if (_impl) vDSP_destroy_fftsetup((FFTSetup)_impl);
}

void RealFft::magnitudes(const float* in, float* mags)
{
    const int half = _n / 2;
    int log2n = 0;
    while ((1 << log2n) < _n) ++log2n;
    DSPSplitComplex split = { _re.data(), _im.data() };
    vDSP_ctoz((const DSPComplex*)in, 2, &split, 1, (vDSP_Length)half);
    vDSP_fft_zrip((FFTSetup)_impl, &split, 1, (vDSP_Length)log2n, FFT_FORWARD);
    split.imagp[0] = 0.f;                     // packed Nyquist term: drop it
    vDSP_zvabs(&split, 1, mags, 1, (vDSP_Length)half);
}

}} // namespace viz::platform
