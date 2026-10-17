#pragma once
#include <complex>
#include <vector>

namespace nf
{
// Minimal iterative radix-2 complex FFT. Forward is unscaled, inverse scales by 1/N.
class FFT
{
public:
    explicit FFT (int order);

    int size() const noexcept { return n; }
    void forward (std::complex<float>* data) const { transform (data, false); }
    void inverse (std::complex<float>* data) const { transform (data, true); }

private:
    void transform (std::complex<float>* data, bool inverse) const;

    int n;
    std::vector<int> bitrev;
    std::vector<std::complex<float>> twiddle;
};
} // namespace nf
