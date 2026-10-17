#include "Spectrogram.h"
#include "FFT.h"
#include "model/Scale.h"
#include <algorithm>
#include <cmath>
#include <complex>

namespace nf
{
// Frequency-reassigned STFT: each bin's energy is drawn at its instantaneous frequency
// (from the window-derivative transform), so harmonics land exactly on their pitch rows
// instead of being quantised to the FFT bin grid. Writes raw magnitudes for columns [c0, c1).
static void reassignedColumns (const float* x, int n, double sr, const SpectrogramData& s, int c0, int c1,
                               std::vector<float>& raw, float& peak)
{
    const int order = sr > 50000.0 ? 13 : 12;
    FFT fft (order);
    const int N = fft.size();
    constexpr float pi = 3.14159265358979f;
    std::vector<std::complex<float>> z ((size_t) N);
    std::vector<float> win ((size_t) N), dwin ((size_t) N);
    for (int i = 0; i < N; ++i)
    {
        win[(size_t) i] = 0.5f - 0.5f * std::cos (2.0f * pi * (float) i / (float) N);
        dwin[(size_t) i] = (pi / (float) N) * std::sin (2.0f * pi * (float) i / (float) N);
    }

    const int cols = c1 - c0;
    raw.assign ((size_t) cols * (size_t) s.height, 0.0f);
    const float minHz = midiToHz (s.minMidi - 1.0f), maxHz = midiToHz (s.maxMidi + 1.0f);
    const int hop = (int) s.hopSamples;
    for (int c = c0; c < c1; ++c)
    {
        const int start = c * hop - N / 2;
        for (int i = 0; i < N; ++i)
        {
            const int k = start + i;
            const float v = (k >= 0 && k < n) ? x[k] : 0.0f;
            z[(size_t) i] = { v * win[(size_t) i], v * dwin[(size_t) i] };
        }
        fft.forward (z.data());
        const int kLo = std::max (1, (int) (minHz * (float) N / (float) sr) - 2);
        const int kHi = std::min (N / 2 - 1, (int) (maxHz * (float) N / (float) sr) + 2);
        for (int k = kLo; k <= kHi; ++k)
        {
            const auto zk = z[(size_t) k];
            const auto zc = std::conj (z[(size_t) (N - k)]);
            const auto A = (zk + zc) * 0.5f;                                 // window
            const auto B = (zk - zc) * std::complex<float> (0.0f, -0.5f);    // window derivative
            const float mag = std::abs (A);
            if (mag < 1.0e-7f)
                continue;
            const float omega = 2.0f * pi * (float) k / (float) N - (B / A).imag();
            const float hz = omega * (float) sr / (2.0f * pi);
            if (hz < minHz || hz > maxHz)
                continue;
            const int r = (int) std::floor ((s.maxMidi - hzToMidi (hz)) * (float) s.rowsPerSemitone);
            if (r < 0 || r >= s.height)
                continue;
            auto& cell = raw[(size_t) r * (size_t) cols + (size_t) (c - c0)];
            cell = std::max (cell, mag);
            peak = std::max (peak, mag);
        }
    }
}

static void toLevels (SpectrogramData& s, const std::vector<float>& raw, int c0, int c1)
{
    constexpr float rangeDb = 66.0f;
    const int cols = c1 - c0;
    const float peak = std::max (s.peak, 1.0e-9f);
    for (int r = 0; r < s.height; ++r)
    {
        for (int c = 0; c < cols; ++c)
        {
            // thicken lines by one row on each side so harmonics stay visible when zoomed out
            float v = raw[(size_t) r * (size_t) cols + (size_t) c];
            if (r > 0) v = std::max (v, 0.5f * raw[(size_t) (r - 1) * (size_t) cols + (size_t) c]);
            if (r + 1 < s.height) v = std::max (v, 0.5f * raw[(size_t) (r + 1) * (size_t) cols + (size_t) c]);
            const float db = 20.0f * std::log10 (v / peak + 1.0e-9f);
            s.level[(size_t) r * (size_t) s.width + (size_t) (c0 + c)] = std::clamp (1.0f + db / rangeDb, 0.0f, 1.0f);
        }
    }
}

SpectrogramData computeSpectrogram (const float* x, int n, double sr, int hop, float minMidi, float maxMidi, int rps)
{
    SpectrogramData s;
    s.hopSamples = hop;
    s.minMidi = minMidi;
    s.maxMidi = maxMidi;
    s.rowsPerSemitone = rps;
    s.width = std::max (1, n / hop + 1);
    s.height = (int) std::lround ((maxMidi - minMidi) * rps);
    s.level.assign ((size_t) s.width * (size_t) s.height, 0.0f);

    std::vector<float> raw;
    float peak = 1.0e-9f;
    reassignedColumns (x, n, sr, s, 0, s.width, raw, peak);
    s.peak = peak;
    toLevels (s, raw, 0, s.width);
    return s;
}

void updateSpectrogram (SpectrogramData& s, const float* x, int n, double sr, int c0, int c1)
{
    c0 = std::clamp (c0, 0, s.width);
    c1 = std::clamp (c1, c0, s.width);
    if (c0 >= c1)
        return;
    std::vector<float> raw;
    float peak = 0.0f;
    reassignedColumns (x, n, sr, s, c0, c1, raw, peak);
    toLevels (s, raw, c0, c1);
}
} // namespace nf
