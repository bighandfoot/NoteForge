#include "Align.h"
#include "FFT.h"
#include <algorithm>
#include <cmath>
#include <complex>
#include <vector>

namespace nf
{
static std::vector<float> decimate (const float* x, int n, int factor)
{
    std::vector<float> out ((size_t) (n / factor));
    for (size_t i = 0; i < out.size(); ++i)
    {
        float s = 0.0f;
        for (int j = 0; j < factor; ++j) s += x[i * (size_t) factor + (size_t) j];
        out[i] = s / (float) factor;
    }
    return out;
}

// Normalised cross-correlation of chunk against the take at one integer offset (full rate).
static double nccAt (const float* take, int n, const float* chunk, int m, long long offset)
{
    double xy = 0, xx = 0, yy = 0;
    for (int i = 0; i < m; ++i)
    {
        const long long k = offset + i;
        const double t = (k >= 0 && k < n) ? take[k] : 0.0;
        xy += t * chunk[i]; xx += t * t; yy += (double) chunk[i] * chunk[i];
    }
    return (xx > 0 && yy > 0) ? xy / std::sqrt (xx * yy) : 0.0;
}

AlignResult findChunkInTake (const float* take, int n, const float* chunk, int m)
{
    AlignResult res;
    if (n <= 0 || m <= 0)
        return res;

    // ---- coarse: decimated by 16, normalised cross-correlation for every offset via one FFT
    constexpr int D = 16;
    const auto t = decimate (take, n, D), c = decimate (chunk, m, D);
    const int N = (int) t.size(), M = (int) c.size();
    if (N < 4 || M < 4)
        return res;
    // allow the chunk to start up to its own length before the take (zero padding at the front)
    std::vector<float> tp ((size_t) (M + N + M), 0.0f);
    std::copy (t.begin(), t.end(), tp.begin() + M);
    const int L = (int) tp.size();
    int order = 1;
    while ((1 << order) < L + M) ++order;
    FFT fft (order);
    const int F = fft.size();
    std::vector<std::complex<float>> A ((size_t) F), B ((size_t) F);
    for (int i = 0; i < L; ++i) A[(size_t) i] = { tp[(size_t) i], 0.0f };
    for (int i = 0; i < M; ++i) B[(size_t) i] = { c[(size_t) i], 0.0f };
    fft.forward (A.data());
    fft.forward (B.data());
    for (int i = 0; i < F; ++i) A[(size_t) i] *= std::conj (B[(size_t) i]);
    fft.inverse (A.data()); // A[k] = sum_i tp[k + i] * c[i]

    std::vector<double> prefix ((size_t) L + 1, 0.0);
    for (int i = 0; i < L; ++i) prefix[(size_t) i + 1] = prefix[(size_t) i] + (double) tp[(size_t) i] * tp[(size_t) i];
    double ec = 0;
    for (float v : c) ec += (double) v * v;
    double best = -2.0;
    int bestK = 0;
    for (int k = 0; k + M <= L; ++k)
    {
        const double et = prefix[(size_t) (k + M)] - prefix[(size_t) k];
        if (et <= 1e-9 * M) continue;
        const double ncc = A[(size_t) k].real() / std::sqrt (et * ec + 1e-30);
        if (ncc > best) { best = ncc; bestK = k; }
    }

    // ---- fine: full rate around the coarse hit, on up to ~2 s of the chunk
    const long long coarse = (long long) (bestK - M) * D;
    const int mFine = std::min (m, 96000);
    double fineBest = -2.0;
    long long fineK = coarse;
    for (long long off = coarse - 2 * D; off <= coarse + 2 * D; ++off)
    {
        const double v = nccAt (take, n, chunk, mFine, off);
        if (v > fineBest) { fineBest = v; fineK = off; }
    }
    res.offset = fineK;
    res.confidence = (float) fineBest;
    return res;
}
} // namespace nf
