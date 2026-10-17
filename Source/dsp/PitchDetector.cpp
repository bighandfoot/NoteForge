#include "PitchDetector.h"
#include <algorithm>
#include <cmath>

namespace nf
{
void PitchDetector::prepare (double sampleRate, float minHz, float maxHz, double windowMs)
{
    sr = sampleRate;
    window = (int) std::lround (windowMs * 0.001 * sr);
    tauMax = (int) std::ceil (sr / minHz);
    tauMin = std::max (2, (int) std::floor (sr / maxHz));

    int order = 1;
    while ((1 << order) < window + tauMax + window + 2)
        ++order;
    fft = std::make_unique<FFT> (order);

    const auto n = (size_t) fft->size();
    z.assign (n, {});
    prod.assign (n, {});
    prefix.assign ((size_t) (window + tauMax + 3), 0.0);
    d.assign ((size_t) tauMax + 2, 0.0f);
    cmnd.assign ((size_t) tauMax + 2, 1.0f);
}

PitchDetector::Result PitchDetector::analyze (const float* x)
{
    Result res;
    const int n = fft->size();
    const int blen = window + tauMax + 1;

    double e0 = 0.0;
    prefix[0] = 0.0;
    for (int i = 0; i < blen; ++i)
        prefix[(size_t) i + 1] = prefix[(size_t) i] + (double) x[i] * x[i];
    e0 = prefix[(size_t) window];
    res.rms = (float) std::sqrt (e0 / window);
    if (e0 < 1.0e-10)
        return res;

    // Pack a (first `window` samples) as real part and b (whole lag span) as imaginary part.
    for (int i = 0; i < n; ++i)
    {
        const float a = i < window ? x[i] : 0.0f;
        const float b = i < blen ? x[i] : 0.0f;
        z[(size_t) i] = { a, b };
    }
    fft->forward (z.data());
    for (int k = 0; k < n; ++k)
    {
        const auto zk = z[(size_t) k];
        const auto zc = std::conj (z[(size_t) ((n - k) & (n - 1))]);
        const auto A = (zk + zc) * 0.5f;
        const auto B = (zk - zc) * std::complex<float> (0.0f, -0.5f);
        prod[(size_t) k] = std::conj (A) * B;
    }
    fft->inverse (prod.data());

    // d(tau) = E0 + E_tau - 2 r(tau), then cumulative mean normalisation.
    d[0] = 0.0f;
    cmnd[0] = 1.0f;
    double running = 0.0;
    for (int tau = 1; tau <= tauMax; ++tau)
    {
        const double etau = prefix[(size_t) (tau + window)] - prefix[(size_t) tau];
        const double v = std::max (0.0, e0 + etau - 2.0 * (double) prod[(size_t) tau].real());
        d[(size_t) tau] = (float) v;
        running += v;
        cmnd[(size_t) tau] = running > 0.0 ? (float) (v * tau / running) : 1.0f;
    }

    constexpr float threshold = 0.12f;
    int best = -1;
    for (int tau = tauMin; tau < tauMax; ++tau)
    {
        if (cmnd[(size_t) tau] < threshold)
        {
            while (tau + 1 < tauMax && cmnd[(size_t) tau + 1] < cmnd[(size_t) tau])
                ++tau;
            best = tau;
            break;
        }
    }
    if (best < 0)
    {
        best = tauMin;
        for (int tau = tauMin; tau < tauMax; ++tau)
            if (cmnd[(size_t) tau] < cmnd[(size_t) best])
                best = tau;
    }

    float refined = (float) best;
    if (best > 1 && best < tauMax)
    {
        const float a = cmnd[(size_t) best - 1], b = cmnd[(size_t) best], c = cmnd[(size_t) best + 1];
        const float den = a - 2.0f * b + c;
        if (std::abs (den) > 1.0e-9f)
            refined += std::clamp (0.5f * (a - c) / den, -1.0f, 1.0f);
    }

    res.aperiodicity = cmnd[(size_t) best];
    {
        // best CMNDF near twice the period (search +-6 % to allow for the interpolated estimate)
        const int t2 = (int) std::lround (2.0f * refined);
        if (t2 + 2 < tauMax)
        {
            float m = 1.0e9f;
            for (int t = std::max (tauMin, (int) (t2 * 0.94f)); t <= std::min (tauMax - 1, (int) (t2 * 1.06f) + 1); ++t)
                m = std::min (m, cmnd[(size_t) t]);
            res.aperiodicity2 = m;
        }
    }
    res.freqHz = (float) (sr / refined);
    return res;
}
} // namespace nf
