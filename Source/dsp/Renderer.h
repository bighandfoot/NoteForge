#pragma once
#include "model/NoteModel.h"
#include <cmath>
#include <vector>

namespace nf
{
// Pitch-synchronous overlap-add (TD-PSOLA) resynthesis of a whole take.
// Pitch is changed by re-spacing grains, formants by resampling each grain, timing by the TimeMap.
void renderTake (const float* input, const Analysis& a, const std::vector<Note>& notes, std::vector<float>& output);

// Multi-channel version: every channel uses the same grain schedule (so stereo stays coherent).
// Only samples [begin, end) of outputs are written; the result there is identical to a full render.
void renderTake (const std::vector<const float*>& inputs, const Analysis& a, const std::vector<Note>& notes,
                 std::vector<std::vector<float>>& outputs, int begin = 0, int end = -1);

// Grain gain that keeps perceived loudness roughly constant when the pulse rate changes by `ratio`.
inline float psolaLoudnessCompensation (float ratio)
{
    constexpr float exponent = 0.0f;
    return std::pow (ratio, exponent);
}

// Shared grain helper: adds one Hann-windowed grain taken around `srcCenter` (asymmetric halves
// `left`/`right` in source samples, compressed by `formantRatio`) centred at `dstCenter`.
struct GrainTarget
{
    float* acc;
    float* wsum;
    long long size;    // number of addressable samples (for linear buffers) or ring size
    long long mask;    // ring mask, or -1 for linear buffers
};

template <typename SampleFn>
inline void addGrain (const GrainTarget& t, SampleFn&& src, double srcCenter, int left, int right,
                      long long dstCenter, float formantRatio, float gain)
{
    const float f = formantRatio;
    const int l = (int) std::ceil (left / f), r = (int) std::ceil (right / f);
    for (int j = -l; j <= r; ++j)
    {
        const double u = (double) j * f;
        const double half = j < 0 ? left : right;
        if (half <= 0.0)
            continue;
        const double ratio = u / half;
        if (ratio <= -1.0 || ratio >= 1.0)
            continue;
        const float w = (float) (0.5 * (1.0 + std::cos (3.14159265358979323846 * ratio)));
        long long pos = dstCenter + j;
        if (t.mask >= 0)
            pos &= t.mask;
        else if (pos < 0 || pos >= t.size)
            continue;
        t.acc[pos] += src (srcCenter + u) * w * gain;
        if (t.wsum != nullptr)
            t.wsum[pos] += w;
    }
}
} // namespace nf
