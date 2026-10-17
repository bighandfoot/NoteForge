#include "Analysis.h"
#include "PitchDetector.h"
#include <algorithm>
#include <cmath>

namespace nf
{
int analysisHopFor (double sr) { return sr > 50000.0 ? 512 : 256; }

// Offline analysis can afford a long window: ~4.5 periods of a low G2, which removes the stepwise
// jitter a 2-period window shows as glottal pulses enter and leave it.
double offlineWindowMs = 46.0;

static void cleanupPitch (Analysis& a)
{
    const int nf = a.numFrames();
    auto& m = a.midi;
    auto& v = a.voiced;

    // 1. octave-error correction against a local median
    const int octRadius = (int) std::lround (0.25 * a.sampleRate / a.hop);
    std::vector<float> fixed = m;
    std::vector<float> window;
    for (int k = 0; k < nf; ++k)
    {
        if (! v[(size_t) k])
            continue;
        window.clear();
        for (int j = std::max (0, k - octRadius); j < std::min (nf, k + octRadius + 1); ++j)
            if (v[(size_t) j]) window.push_back (m[(size_t) j]);
        std::nth_element (window.begin(), window.begin() + (long) window.size() / 2, window.end());
        const float med = window[window.size() / 2];
        float p = m[(size_t) k];
        while (p - med > 9.0f) p -= 12.0f;
        while (med - p > 9.0f) p += 12.0f;
        fixed[(size_t) k] = p;
    }
    m = fixed;

    // 2. 5-point median within voiced runs
    for (int k = 0; k < nf; ++k)
    {
        if (! v[(size_t) k])
            continue;
        float w[5];
        int c = 0;
        for (int j = k - 2; j <= k + 2; ++j)
            if (j >= 0 && j < nf && v[(size_t) j]) w[c++] = m[(size_t) j];
        std::sort (w, w + c);
        fixed[(size_t) k] = w[c / 2];
    }
    m = fixed;

    // 2b. repair pitch at the edges of voiced runs: jumps in the first/last few frames (onset noise,
    //     decaying releases) and the very quiet end of a release hold the last trustworthy value.
    //     Frames stay voiced so edits still apply to the whole note.
    for (int k = 0; k < nf;)
    {
        if (! v[(size_t) k]) { ++k; continue; }
        const int s = k;
        int e = k;
        while (e < nf && v[(size_t) e]) ++e;
        k = e;
        if (e - s < 8) continue;

        for (int j = s + 4; j >= s; --j)
            if (std::abs (m[(size_t) j] - m[(size_t) j + 1]) > 1.2f)
            {
                for (int i = s; i <= j; ++i) m[(size_t) i] = m[(size_t) j + 1];
                break;
            }
        for (int j = e - 5; j < e; ++j)
            if (std::abs (m[(size_t) j] - m[(size_t) j - 1]) > 1.2f)
            {
                for (int i = j; i < e; ++i) m[(size_t) i] = m[(size_t) j - 1];
                break;
            }

        float runMax = 0.0f;
        for (int i = s; i < e; ++i) runMax = std::max (runMax, a.rms[(size_t) i]);
        int t = e - 1;
        while (t > s + 4 && a.rms[(size_t) t] < 0.15f * runMax) --t;
        for (int i = t + 1; i < e; ++i) m[(size_t) i] = m[(size_t) t];
    }

    // 3. bridge short unvoiced gaps whose ends agree in pitch
    const int maxGap = 3;
    for (int k = 1; k < nf; ++k)
    {
        if (v[(size_t) k] || ! v[(size_t) k - 1])
            continue;
        int e = k;
        while (e < nf && ! v[(size_t) e] && e - k < maxGap + 1) ++e;
        if (e < nf && v[(size_t) e] && e - k <= maxGap && std::abs (m[(size_t) e] - m[(size_t) k - 1]) < 2.0f)
        {
            for (int j = k; j < e; ++j)
            {
                const float t = (float) (j - k + 1) / (float) (e - k + 1);
                m[(size_t) j] = m[(size_t) k - 1] + (m[(size_t) e] - m[(size_t) k - 1]) * t;
                v[(size_t) j] = 1;
            }
        }
        k = e;
    }

    // 4. drop tiny voiced islands
    const int minIsland = 4;
    for (int k = 0; k < nf;)
    {
        if (! v[(size_t) k]) { ++k; continue; }
        int e = k;
        while (e < nf && v[(size_t) e]) ++e;
        if (e - k < minIsland)
            for (int j = k; j < e; ++j) v[(size_t) j] = 0;
        k = e;
    }
}

AnalysisResult analyzeTake (const float* x, int n, double sr, std::atomic<float>* progress, const std::atomic<bool>* cancel)
{
    AnalysisResult res;
    auto& a = res.analysis;
    a.sampleRate = sr;
    a.hop = analysisHopFor (sr);
    a.numSamples = n;
    const int nf = n / a.hop + 1;
    a.rms.assign ((size_t) nf, 0.0f);
    a.rawHz.assign ((size_t) nf, 0.0f);
    a.aperiodicity.assign ((size_t) nf, 1.0f);

    computeFrames (a, x, 0, nf, progress, cancel);
    if (cancel && cancel->load())
        return res;
    res.notes = finalizeAnalysis (a, x);
    if (progress)
        progress->store (1.0f);
    return res;
}

void computeFrames (Analysis& a, const float* x, int k0, int k1, std::atomic<float>* progress, const std::atomic<bool>* cancel)
{
    const int n = a.numSamples;
    PitchDetector det;
    det.prepare (a.sampleRate, 55.0f, 1400.0f, offlineWindowMs);
    const int need = det.samplesNeeded();
    std::vector<float> frame ((size_t) need);
    k0 = std::max (0, k0);
    k1 = std::min (k1, (int) a.rms.size());

    for (int k = k0; k < k1; ++k)
    {
        if (cancel && cancel->load())
            return;
        const int start = k * a.hop - det.windowSize() / 2;
        for (int i = 0; i < need; ++i)
        {
            const int s = start + i;
            frame[(size_t) i] = (s >= 0 && s < n) ? x[s] : 0.0f;
        }
        const auto r = det.analyze (frame.data());
        a.rms[(size_t) k] = r.rms;
        a.aperiodicity[(size_t) k] = r.aperiodicity;
        a.rawHz[(size_t) k] = r.freqHz;
        if (progress && (k & 255) == 0)
            progress->store (0.85f * (float) (k - k0) / (float) std::max (1, k1 - k0));
    }
}

std::vector<Note> finalizeAnalysis (Analysis& a, const float* x)
{
    const int nf = (int) a.rms.size();
    a.midi.assign ((size_t) nf, 0.0f);
    a.voiced.assign ((size_t) nf, 0);
    a.maxRms = nf > 0 ? *std::max_element (a.rms.begin(), a.rms.end()) : 0.0f;
    const float gate = std::max (1.0e-4f, a.maxRms * std::pow (10.0f, -42.0f / 20.0f));
    for (int k = 0; k < nf; ++k)
    {
        const float f = a.rawHz[(size_t) k];
        a.voiced[(size_t) k] = (f > 0.0f && a.aperiodicity[(size_t) k] < 0.25f && a.rms[(size_t) k] > gate) ? 1 : 0;
        a.midi[(size_t) k] = f > 0.0f ? hzToMidi (f) : 0.0f;
    }

    cleanupPitch (a);
    computePitchMarks (a, x);
    return segmentNotes (a);
}

void computePitchMarks (Analysis& a, const float* x)
{
    const int n = a.numSamples;
    a.marks.clear();
    a.markVoiced.clear();
    if (n <= 0)
        return;

    // zero-phase low-passed copy for robust pulse picking
    std::vector<float> y ((size_t) n);
    const float alpha = (float) std::exp (-2.0 * 3.14159265358979 * 900.0 / a.sampleRate);
    float s1 = 0.0f, s2 = 0.0f;
    for (int i = 0; i < n; ++i) { s1 = x[i] + alpha * (s1 - x[i]); s2 = s1 + alpha * (s2 - s1); y[(size_t) i] = s2; }
    s1 = s2 = 0.0f;
    for (int i = n - 1; i >= 0; --i) { const float v = y[(size_t) i]; s1 = v + alpha * (s1 - v); s2 = s1 + alpha * (s2 - s1); y[(size_t) i] = s2; }

    const int unvoicedStep = std::max (32, (int) std::lround (0.005 * a.sampleRate));
    auto voicedAt = [&] (int s) { return a.voicedAtFrame ((int) std::lround ((double) s / a.hop)); };
    auto periodAt = [&] (int s) { return a.sampleRate / midiToHz (a.midiAtFrame ((double) s / a.hop)); };
    auto argmax = [&] (int lo, int hi)
    {
        lo = std::clamp (lo, 0, n - 1);
        hi = std::clamp (hi, 0, n - 1);
        int best = lo;
        for (int i = lo; i <= hi; ++i)
            if (y[(size_t) i] > y[(size_t) best]) best = i;
        return best;
    };

    int t = 0;
    bool prevVoiced = false;
    a.marks.push_back (0);
    a.markVoiced.push_back (voicedAt (0) ? 1 : 0);
    while (t < n - 1)
    {
        int next;
        const bool v = voicedAt (t);
        if (v)
        {
            const double p = periodAt (t);
            if (prevVoiced)
            {
                const double cand = t + p;
                next = argmax ((int) (cand - 0.2 * p), (int) (cand + 0.2 * p));
            }
            else
                next = argmax (t + 1, (int) (t + p));
            next = std::max (next, t + std::max (2, (int) (0.5 * p)));
        }
        else
            next = t + unvoicedStep;

        next = std::min (next, n - 1);
        if (next <= t)
            break;
        const bool nv = voicedAt (next);
        a.marks.push_back (next);
        a.markVoiced.push_back (nv ? 1 : 0);
        prevVoiced = v && nv;
        t = next;
    }
}

std::vector<Note> segmentNotes (const Analysis& a)
{
    std::vector<Note> notes;
    const int nf = a.numFrames();
    const double fps = a.sampleRate / a.hop;
    const int smoothR = std::max (1, (int) std::lround (0.075 * fps));
    const int sustain = std::max (2, (int) std::lround (0.05 * fps));
    const int minLen = std::max (4, (int) std::lround (0.06 * fps));
    const int dipR = std::max (3, (int) std::lround (0.09 * fps));

    std::vector<float> db ((size_t) nf);
    for (int k = 0; k < nf; ++k)
        db[(size_t) k] = 20.0f * std::log10 (a.rms[(size_t) k] + 1.0e-7f);

    for (int k = 0; k < nf;)
    {
        if (! a.voiced[(size_t) k]) { ++k; continue; }
        const int runStart = k;
        int runEnd = k;
        while (runEnd < nf && a.voiced[(size_t) runEnd]) ++runEnd;
        k = runEnd;

        const int len = runEnd - runStart;
        if (len < minLen / 2 + 2)
            continue;

        // vibrato-suppressed pitch
        std::vector<float> s ((size_t) len);
        for (int i = 0; i < len; ++i)
        {
            double sum = 0; int c = 0;
            for (int j = std::max (0, i - smoothR); j <= std::min (len - 1, i + smoothR); ++j) { sum += a.midi[(size_t) (runStart + j)]; ++c; }
            s[(size_t) i] = (float) (sum / c);
        }

        std::vector<int> splits;
        // pitch-change splits
        int noteStart = 0, count = 0, skipUntil = 0;
        double refSum = 0.0; int refCnt = 0;
        for (int i = 0; i < len; ++i)
        {
            if (i < skipUntil) continue; // still inside the transition into this note
            if (refCnt == 0) { refSum = s[(size_t) i]; refCnt = 1; continue; }
            const float ref = (float) (refSum / refCnt);
            if (std::abs (s[(size_t) i] - ref) > 0.7f) ++count;
            else { count = 0; refSum += s[(size_t) i]; ++refCnt; }

            if (count >= sustain)
            {
                const int onset = i - count + 1;
                int best = onset;
                float bestSlope = -1.0f;
                for (int j = std::max (1, onset - sustain); j <= std::min (len - 2, onset + sustain / 2); ++j)
                {
                    const float slope = std::abs (s[(size_t) j + 1] - s[(size_t) j - 1]);
                    if (slope > bestSlope) { bestSlope = slope; best = j; }
                }
                if (best - noteStart >= minLen && len - best >= minLen)
                {
                    splits.push_back (best);
                    noteStart = best;
                }
                // re-anchor once the glide has settled
                refSum = 0.0; refCnt = 0;
                skipUntil = i + 1;
                while (skipUntil + 1 < len && std::abs (s[(size_t) skipUntil + 1] - s[(size_t) skipUntil - 1]) > 0.08f)
                    ++skipUntil;
                count = 0;
            }
        }

        // loudness-dip splits (re-articulated notes at the same pitch)
        for (int i = dipR; i < len - dipR; ++i)
        {
            const int kk = runStart + i;
            bool isMin = true;
            for (int j = kk - 2; j <= kk + 2 && isMin; ++j)
                if (db[(size_t) j] < db[(size_t) kk]) isMin = false;
            if (! isMin) continue;
            float before = -200.0f, after = -200.0f;
            for (int j = kk - dipR; j < kk; ++j) before = std::max (before, db[(size_t) j]);
            for (int j = kk + 1; j <= kk + dipR; ++j) after = std::max (after, db[(size_t) j]);
            if (std::min (before, after) - db[(size_t) kk] < 8.0f) continue;
            bool farEnough = i >= minLen && len - i >= minLen;
            for (int sp : splits) if (std::abs (sp - i) < minLen) farEnough = false;
            if (farEnough) splits.push_back (i);
        }

        std::sort (splits.begin(), splits.end());
        std::vector<int> bounds { 0 };
        for (int sp : splits) bounds.push_back (sp);
        bounds.push_back (len);

        // fold short transition segments into the note they lead to (or the previous one at the end)
        const int minKeep = std::max (minLen, (int) std::lround (0.09 * fps));
        for (bool changed = true; changed && bounds.size() > 2;)
        {
            changed = false;
            for (size_t b = 0; b + 1 < bounds.size(); ++b)
            {
                if (bounds[b + 1] - bounds[b] >= minKeep)
                    continue;
                if (b + 2 < bounds.size()) bounds.erase (bounds.begin() + (long) b + 1); // merge with next
                else bounds.erase (bounds.begin() + (long) b);                       // merge with previous
                changed = true;
                break;
            }
        }

        for (size_t b = 0; b + 1 < bounds.size(); ++b)
        {
            Note n;
            n.startFrame = runStart + bounds[b];
            n.endFrame = runStart + bounds[b + 1];
            n.origCenter = computeCenter (a, n.startFrame, n.endFrame);
            n.outStart = n.inStart (a.hop);
            n.outEnd = n.inEnd (a.hop);
            notes.push_back (n);
        }
    }

    // Drop "notes" far quieter than the singing (background noise, hum, bleed): more than 18 dB below
    // the median note level. Their frames stay visible as unassigned pitch but are never edited.
    if (notes.size() >= 3)
    {
        std::vector<float> levels;
        for (auto& n : notes)
        {
            double s = 0;
            for (int k = n.startFrame; k < n.endFrame; ++k) s += a.rms[(size_t) k];
            levels.push_back ((float) (s / std::max (1, n.endFrame - n.startFrame)));
        }
        auto sorted = levels;
        std::nth_element (sorted.begin(), sorted.begin() + (long) sorted.size() / 2, sorted.end());
        const float floorLevel = sorted[sorted.size() / 2] * std::pow (10.0f, -18.0f / 20.0f);
        std::vector<Note> kept;
        for (size_t i = 0; i < notes.size(); ++i)
            if (levels[i] >= floorLevel) kept.push_back (notes[i]);
        notes = std::move (kept);
    }
    return notes;
}
} // namespace nf
