#include "Renderer.h"
#include <algorithm>
#include <cmath>

namespace nf
{
static float interpCurve (const std::vector<float>& c, double frame)
{
    if (c.empty())
        return 0.0f;
    const int k0 = std::clamp ((int) std::floor (frame), 0, (int) c.size() - 1);
    const int k1 = std::min (k0 + 1, (int) c.size() - 1);
    const float t = (float) std::clamp (frame - k0, 0.0, 1.0);
    return c[(size_t) k0] + (c[(size_t) k1] - c[(size_t) k0]) * t;
}

void renderTake (const float* x, const Analysis& a, const std::vector<Note>& notes, std::vector<float>& out)
{
    std::vector<std::vector<float>> outs;
    renderTake ({ x }, a, notes, outs);
    out = std::move (outs[0]);
}

void renderTake (const std::vector<const float*>& in, const Analysis& a, const std::vector<Note>& notes,
                 std::vector<std::vector<float>>& out, int begin, int end)
{
    const int n = a.numSamples;
    const int numCh = (int) in.size();
    out.resize ((size_t) numCh);
    for (auto& o : out)
        if ((int) o.size() != n) o.assign ((size_t) std::max (0, n), 0.0f);
    if (end < 0 || end > n) end = n;
    begin = std::clamp (begin, 0, n);
    if (n <= 0 || begin >= end)
        return;
    if (a.marks.size() < 2)
    {
        for (int c = 0; c < numCh; ++c)
            std::copy (in[(size_t) c] + begin, in[(size_t) c] + end, out[(size_t) c].begin() + begin);
        return;
    }

    const auto curves = buildCurves (a, notes);
    TimeMap tm;
    tm.build (notes, a.hop, n);
    const int hop = a.hop;

    // Work buffer covering the requested range plus the context the loudness matcher needs.
    const int lo = std::max (0, begin - 3 * hop), hi = std::min (n, end + 3 * hop);
    const int len = hi - lo;
    std::vector<std::vector<float>> acc ((size_t) numCh, std::vector<float> ((size_t) len, 0.0f));
    std::vector<float> wsum ((size_t) len, 0.0f);

    const auto& marks = a.marks;
    const int numMarks = (int) marks.size();
    size_t m = 0;
    double om = 0.0;
    // The grain chain is always walked from the start so a partial render is sample-identical
    // to the same range of a full render; only grains that touch [lo, hi) are actually added.
    while (om < n)
    {
        const double ti = tm.toInput (om);
        while (m + 1 < (size_t) numMarks && marks[m + 1] <= ti)
            ++m;
        size_t pick = m;
        if (m + 1 < (size_t) numMarks && (marks[m + 1] - ti) < (ti - marks[m]))
            pick = m + 1;

        const int c = marks[pick];
        const int right = pick + 1 < (size_t) numMarks ? marks[pick + 1] - c : (pick > 0 ? c - marks[pick - 1] : 256);
        const int left = pick > 0 ? c - marks[pick - 1] : right;

        const double frame = ti / hop;
        const bool voiced = a.markVoiced[pick] != 0;
        const float shift = interpCurve (curves.shift, frame);
        const float ratio = voiced ? std::exp2 (shift / 12.0f) : 1.0f;
        const float formantRatio = std::exp2 (interpCurve (curves.formant, frame) / 12.0f);

        const long long centre = (long long) std::llround (om);
        const double reachL = left / formantRatio + 2.0, reachR = right / formantRatio + 2.0;
        if ((double) centre + reachR >= lo && (double) centre - reachL < hi)
        {
            float gain = interpCurve (curves.gain, frame);
            if (voiced)
                gain *= psolaLoudnessCompensation (ratio);
            for (int ch = 0; ch < numCh; ++ch)
            {
                const float* x = in[(size_t) ch];
                auto sample = [x, n] (double pos)
                {
                    const int i = (int) std::floor (pos);
                    const float fr = (float) (pos - i);
                    const float s0 = (i >= 0 && i < n) ? x[i] : 0.0f;
                    const float s1 = (i + 1 >= 0 && i + 1 < n) ? x[i + 1] : 0.0f;
                    return s0 + (s1 - s0) * fr;
                };
                // the window sum is the same for every channel, so only the first one accumulates it
                const GrainTarget t { acc[(size_t) ch].data(), ch == 0 ? wsum.data() : nullptr, len, -1 };
                addGrain (t, sample, (double) c, left, right, centre - lo, formantRatio, gain);
            }
        }

        double next = om + std::max (1.0, (double) right / ratio);
        // Where nothing changes pitch and time is not stretched, gently re-lock the grain chain onto
        // the (time-mapped) analysis marks. This removes the sub-period offset an earlier edit leaves
        // behind, so unedited material is reproduced exactly and partial renders join seamlessly.
        if (ratio == 1.0f && pick + 1 < (size_t) numMarks)
        {
            const double here = tm.toOutput ((double) c), target = tm.toOutput ((double) marks[pick + 1]);
            if (std::abs ((target - here) - right) < 1.0)
                next = om + right + std::clamp (target - (om + right), -0.08 * right, 0.08 * right);
        }
        om = std::max (om + 1.0, next);
    }

    for (int ch = 0; ch < numCh; ++ch)
        for (int i = 0; i < len; ++i)
            acc[(size_t) ch][(size_t) i] /= std::max (wsum[(size_t) i], 1.0f);

    // Loudness preservation: PSOLA changes the pulse density, so match the short-time RMS of the
    // output to that of the source material it was taken from (times the user's gain curve).
    const int win = hop * 2;
    auto energyAround = [&] (bool input, double centre)
    {
        const int s = std::max (input ? 0 : lo, (int) centre - win / 2);
        const int e = std::min (input ? n : hi, (int) centre + win / 2);
        double sum = 0.0;
        for (int ch = 0; ch < numCh; ++ch)
            for (int i = s; i < e; ++i)
            {
                const float v = input ? in[(size_t) ch][i] : acc[(size_t) ch][(size_t) (i - lo)];
                sum += (double) v * v;
            }
        return e > s ? std::sqrt (sum / ((e - s) * numCh)) : 0.0;
    };
    const int kb = std::max (0, begin / hop - 2), ke = std::min (n / hop + 1, end / hop + 3);
    std::vector<float> corr ((size_t) (ke - kb), 1.0f);
    for (int k = kb; k < ke; ++k)
    {
        const double t = (double) k * hop;
        const double ti = tm.toInput (t);
        const double desired = energyAround (true, ti) * interpCurve (curves.gain, ti / hop);
        const double actual = energyAround (false, t);
        if (actual > 1.0e-5 && desired > 1.0e-5)
            corr[(size_t) (k - kb)] = (float) std::clamp (desired / actual, 0.5, 2.5);
    }
    std::vector<float> smooth = corr;
    for (size_t k = 1; k + 1 < corr.size(); ++k)
        smooth[k] = 0.25f * corr[k - 1] + 0.5f * corr[k] + 0.25f * corr[k + 1];

    for (int ch = 0; ch < numCh; ++ch)
        for (int i = begin; i < end; ++i)
            out[(size_t) ch][(size_t) i] = acc[(size_t) ch][(size_t) (i - lo)] * interpCurve (smooth, (double) i / hop - kb);
}
} // namespace nf
