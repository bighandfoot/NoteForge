#include "FFT.h"
#include <cmath>
#include <utility>

namespace nf
{
FFT::FFT (int order) : n (1 << order), bitrev ((size_t) n), twiddle ((size_t) n / 2)
{
    for (int i = 0; i < n; ++i)
    {
        int r = 0;
        for (int b = 0; b < order; ++b)
            if (i & (1 << b))
                r |= 1 << (order - 1 - b);
        bitrev[(size_t) i] = r;
    }
    for (int i = 0; i < n / 2; ++i)
    {
        const double a = -2.0 * 3.14159265358979323846 * i / n;
        twiddle[(size_t) i] = { (float) std::cos (a), (float) std::sin (a) };
    }
}

void FFT::transform (std::complex<float>* d, bool inv) const
{
    for (int i = 0; i < n; ++i)
        if (i < bitrev[(size_t) i])
            std::swap (d[i], d[bitrev[(size_t) i]]);

    for (int len = 2; len <= n; len <<= 1)
    {
        const int half = len / 2;
        const int step = n / len;
        for (int i = 0; i < n; i += len)
        {
            for (int j = 0; j < half; ++j)
            {
                auto w = twiddle[(size_t) (j * step)];
                if (inv)
                    w = std::conj (w);
                const auto u = d[i + j];
                const auto v = d[i + j + half] * w;
                d[i + j] = u + v;
                d[i + j + half] = u - v;
            }
        }
    }

    if (inv)
    {
        const float s = 1.0f / (float) n;
        for (int i = 0; i < n; ++i)
            d[i] *= s;
    }
}
} // namespace nf
