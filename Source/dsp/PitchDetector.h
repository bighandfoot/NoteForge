#pragma once
#include "FFT.h"
#include <complex>
#include <memory>
#include <vector>

namespace nf
{
// YIN fundamental-frequency estimator. The difference function is computed through an
// FFT cross-correlation, so a frame costs two FFTs regardless of the lag range.
class PitchDetector
{
public:
    struct Result
    {
        float freqHz = 0.0f;       // 0 when no period was found
        float aperiodicity = 1.0f; // CMNDF value at the chosen lag (0 = perfectly periodic)
        float rms = 0.0f;
        // CMNDF at twice the chosen period (or -1 if out of range). A real voice repeats over two periods
        // (low value); narrow-band noise such as a whistly breath only looks periodic over one (high value).
        float aperiodicity2 = -1.0f;
    };

    // windowMs: analysis window; longer = smoother tracking of low voices, shorter = faster response.
    void prepare (double sampleRate, float minHz = 55.0f, float maxHz = 1400.0f, double windowMs = 23.0);

    int windowSize() const noexcept { return window; }
    int maxLag() const noexcept { return tauMax; }
    // Samples required starting at the frame pointer. A frame "centred" at c starts at c - windowSize()/2.
    int samplesNeeded() const noexcept { return window + tauMax + 2; }

    Result analyze (const float* x);

private:
    double sr = 44100.0;
    int window = 1024, tauMin = 32, tauMax = 800;
    std::unique_ptr<FFT> fft;
    std::vector<std::complex<float>> z, prod;
    std::vector<double> prefix;
    std::vector<float> d, cmnd;
};
} // namespace nf
