//
// fft_selftest.cpp — the linked RealFft (vDSP on Apple, ours elsewhere)
// against a naive DFT in vDSP's 2x magnitude convention, plus (on Apple) the
// Analyzer's normalized Hann window against vDSP's. Keeps every platform's
// spectrum calibration identical.
//
#include "../platform/fft.h"
#include <cmath>
#include <cstdio>
#include <vector>
#ifdef __APPLE__
#include <Accelerate/Accelerate.h>
#endif

int main()
{
    const int N = 1024;
    std::vector<float> x(N);
    for (int i = 0; i < N; ++i)
        x[i] = 0.5f * std::sin(2 * M_PI * 60.0 * i / 48000.0) + 0.2f * std::sin(2 * M_PI * 3100.0 * i / 48000.0)
             + 0.05f * (float)((i * 7919) % 97) / 97.f;

    std::vector<float> mags(N / 2), ref(N / 2);
    viz::platform::RealFft fft(N);
    fft.magnitudes(x.data(), mags.data());
    for (int k = 0; k < N / 2; ++k) {
        double re = 0, im = 0;
        for (int n = 0; n < N; ++n) {
            double a = -2 * M_PI * k * n / N;
            re += x[n] * std::cos(a); im += x[n] * std::sin(a);
        }
        ref[k] = 2.f * (float)std::sqrt(re * re + (k ? im * im : 0.0));
    }
    double maxErr = 0, maxRef = 0;
    for (int k = 0; k < N / 2; ++k) {
        maxErr = std::max(maxErr, (double)std::fabs(mags[k] - ref[k]));
        maxRef = std::max(maxRef, (double)ref[k]);
    }
    int fails = 0;
    printf("fft: max |err| = %.3e (max magnitude %.3f)\n", maxErr, maxRef);
    if (maxErr > 1e-3 * maxRef) { printf("  [FAIL] RealFft deviates from the DFT\n"); fails++; }
    else printf("  [ok] RealFft matches the DFT within 0.1%%\n");

#ifdef __APPLE__
    std::vector<float> ours(N), vd(N);
    const float norm = std::sqrt(8.f / 3.f);
    for (int i = 0; i < N; ++i) ours[i] = norm * 0.5f * (1.f - std::cos(2.f * (float)M_PI * i / (float)N));
    vDSP_hann_window(vd.data(), N, vDSP_HANN_NORM);
    double werr = 0;
    for (int i = 0; i < N; ++i) werr = std::max(werr, (double)std::fabs(ours[i] - vd[i]));
    printf("window: max |ours - vDSP_HANN_NORM| = %.3e\n", werr);
    if (werr > 1e-4) { printf("  [FAIL] window mismatch\n"); fails++; }
    else printf("  [ok] normalized Hann matches vDSP\n");
#endif
    printf("fft_selftest: %s\n", fails ? "FAILED" : "PASS");
    return fails ? 1 : 0;
}
