#include "LiveTuner.h"
#include "Renderer.h"
#include <algorithm>
#include <cmath>

namespace nf
{
void LiveTuner::prepare (double sampleRate, float lowestHz)
{
    sr = sampleRate;
    hop = sr > 50000.0 ? 512 : 256;
    lowest = lowestHz;
    detector.prepare (sr, lowestHz);
    probing = lowestHz > autoRangeHz + 5.0f;
    if (probing)
        probe.prepare (sr, 50.0f);
    maxPeriod = detector.maxLag();
    unvoicedStep = std::max (32, (int) std::lround (0.004 * sr));
    // High quality: pitch frames are causal (estimated from the most recent window), so the delay only has
    // to cover the next pitch mark (<= 1.2 periods), one grain of synthesis slack (<= 1.2 periods) and the
    // left reach of a grain (<= 1.2 periods / minimum formant ratio 0.75).
    latencyHigh = (int) std::ceil (4.2 * maxPeriod) + 64;
    // Low latency: the read tap wanders within half a period either side of this delay.
    latencyLow = (int) std::ceil (0.5 * maxPeriod) + (int) std::lround (0.001 * sr) + 4;
    lpAlpha = (float) std::exp (-2.0 * 3.14159265358979 * 900.0 / sr);
    mixAlpha = (float) (1.0 - std::exp (-1.0 / (0.012 * sr))); // ~12 ms cross-fade for bypass / mix moves

    in.assign ((size_t) ringSize, 0.0f);
    lp.assign ((size_t) ringSize, 0.0f);
    wsum.assign ((size_t) ringSize, 0.0f);
    for (auto& v : inCh) v.assign ((size_t) ringSize, 0.0f);
    for (auto& v : accCh) v.assign ((size_t) ringSize, 0.0f);
    frames.assign (frameRing, 0.0f);
    framesVoiced.assign (frameRing, 0);
    scratch.assign ((size_t) std::max (detector.samplesNeeded(), probing ? probe.samplesNeeded() : 0), 0.0f);
    reset();
}

void LiveTuner::reset()
{
    std::fill (in.begin(), in.end(), 0.0f);
    std::fill (lp.begin(), lp.end(), 0.0f);
    std::fill (wsum.begin(), wsum.end(), 0.0f);
    for (auto& v : inCh) std::fill (v.begin(), v.end(), 0.0f);
    for (auto& v : accCh) std::fill (v.begin(), v.end(), 0.0f);
    std::fill (framesVoiced.begin(), framesVoiced.end(), (uint8_t) 0);
    written = 0;
    nextFrame = 0;
    lp1 = lp2 = 0.0f;
    marks.clear();
    marks.push_back ({ 0, false });
    lastMarkVoiced = false;
    outMark = 0.0;
    delay = oldDelay = (double) latencyLow;
    lastPeriod = 0.5 * maxPeriod;
    fadePos = fadeLen = 0;
    llShift = 0.0f;
    smoothShift = 0.0f;
    currentTarget = -1000;
    fillCount = 0;
    onsetPending = false;
    onsetMidi = 0.0f;
}

void LiveTuner::resetAfterGap()
{
    // keep the running sample counter (so frame/mark bookkeeping stays monotonic), forget all content
    std::fill (in.begin(), in.end(), 0.0f);
    std::fill (lp.begin(), lp.end(), 0.0f);
    std::fill (wsum.begin(), wsum.end(), 0.0f);
    for (auto& v : inCh) std::fill (v.begin(), v.end(), 0.0f);
    for (auto& v : accCh) std::fill (v.begin(), v.end(), 0.0f);
    std::fill (framesVoiced.begin(), framesVoiced.end(), (uint8_t) 0);
    nextFrame = written / hop + 1;
    lp1 = lp2 = 0.0f;
    marks.clear();
    marks.push_back ({ written, false });
    lastMarkVoiced = false;
    outMark = (double) written;
    delay = oldDelay = (double) latencyLow;
    fadePos = fadeLen = 0;
    llShift = 0.0f;
    smoothShift = 0.0f;
    currentTarget = -1000;
    fillCount = 0;
    onsetPending = false;
}

float LiveTuner::frameMidi (double frame) const
{
    const long long k0 = std::max (0ll, (long long) std::floor (frame));
    const long long k1 = k0 + 1;
    const bool v0 = frameVoiced (k0), v1 = frameVoiced (k1) && k1 < nextFrame;
    const float m0 = frames[(size_t) (k0 % frameRing)], m1 = frames[(size_t) (k1 % frameRing)];
    const float t = (float) (frame - (double) k0);
    if (v0 && v1) return m0 + (m1 - m0) * t;
    if (v0) return m0;
    if (v1) return m1;
    return 0.0f;
}

bool LiveTuner::frameVoiced (long long k) const
{
    return k >= 0 && k < nextFrame && k > nextFrame - frameRing && framesVoiced[(size_t) (k % frameRing)] != 0;
}

float LiveTuner::readAt (int ch, double pos) const
{
    // 4-point cubic (Catmull-Rom) read from a channel ring
    const auto& r = inCh[(size_t) ch];
    const long long i = (long long) std::floor (pos);
    const float t = (float) (pos - (double) i);
    auto at = [&] (long long k) { return k < 0 ? 0.0f : r[(size_t) (k & ringMask)]; };
    const float y0 = at (i - 1), y1 = at (i), y2 = at (i + 1), y3 = at (i + 2);
    return y1 + 0.5f * t * (y2 - y0 + t * (2.0f * y0 - 5.0f * y1 + 4.0f * y2 - y3 + t * (3.0f * (y1 - y2) + y3 - y0)));
}

void LiveTuner::analyseFrames()
{
    const int need = detector.samplesNeeded();
    const int fill = (int) scratch.size(); // >= need; longer when the below-range probe is active
    while (nextFrame * hop <= written)
    {
        const long long start = nextFrame * hop - fill; // the window that ends at this frame's time
        for (int i = 0; i < fill; ++i)
        {
            const long long s = start + i;
            scratch[(size_t) i] = s >= 0 ? in[(size_t) (s & ringMask)] : 0.0f;
        }
        const auto r = detector.analyze (scratch.data() + (fill - need));
        const bool prevVoiced = frameVoiced (nextFrame - 1);
        const float prev = prevVoiced ? frames[(size_t) ((nextFrame - 1) % frameRing)] : 0.0f;
        // Two-period check: real singing repeats over two cycles; whistly breath/noise only looks periodic
        // over one and would otherwise be chopped into tiny grains (heard as static).
        const float rawMidi = r.freqHz > 0.0f ? hzToMidi (r.freqHz) : 0.0f;
        const bool twoOk = r.aperiodicity2 < 0.0f || r.aperiodicity2 < (prevVoiced ? 0.3f : 0.15f);
        bool v;
        if (prevVoiced)
        {
            // hysteresis: a note that is already being tracked may get breathier/quieter before it is let go
            v = r.freqHz > 0.0f && r.rms > 0.002f && r.aperiodicity < 0.3f && twoOk;
        }
        else
        {
            // a new note must look like a note on two consecutive frames at the same pitch
            const bool candidate = r.freqHz > 0.0f && r.rms > 0.002f && r.aperiodicity < 0.2f && twoOk;
            v = candidate && onsetPending && std::abs (rawMidi - onsetMidi) < 1.0f;
            onsetPending = candidate && ! v;
            onsetMidi = rawMidi;
        }
        if (v)
            onsetPending = false;
        float midi = v ? rawMidi : 0.0f;
        // light octave-jump guard against the previous voiced frame
        if (v && prevVoiced)
        {
            if (midi - prev > 9.0f) midi -= 12.0f;
            else if (prev - midi > 9.0f) midi += 12.0f;
        }
        if (v)
            fillCount = 0;
        else if (prevVoiced && r.rms > 0.004f && fillCount < 3 && r.freqHz > 0.0f && r.aperiodicity < 0.45f && twoOk)
        {
            // bridge a short tracking dropout inside a sung note instead of switching correction off
            // (only while the sound is still somewhat periodic: breath noise must never be retuned as a note)
            v = true;
            midi = prev;
            ++fillCount;
        }
        else if (probing && r.rms > 0.01f && (nextFrame & 3) == 0)
        {
            // loud but untracked: is the singer below the selected voice range?
            const auto pr = probe.analyze (scratch.data() + (fill - probe.samplesNeeded()));
            if (pr.freqHz > 0.0f && pr.freqHz < lowest * 1.05f && pr.aperiodicity < 0.25f)
                notesBelowRange.fetch_add (1, std::memory_order_relaxed);
        }
        frames[(size_t) (nextFrame % frameRing)] = midi;
        framesVoiced[(size_t) (nextFrame % frameRing)] = v ? 1 : 0;
        framesAnalysed.fetch_add (1, std::memory_order_relaxed);
        if (v) voicedFrameCount.fetch_add (1, std::memory_order_relaxed);
        ++nextFrame;
    }
}

float LiveTuner::targetShift (float detected, const Params& p)
{
    const float nearest = p.scale.snap (detected);
    if (currentTarget < -100 || std::abs (detected - (float) currentTarget) > 1.5f)
        currentTarget = (int) nearest;
    else if ((int) nearest != currentTarget
             && std::abs (detected - nearest) + 0.15f < std::abs (detected - (float) currentTarget))
        currentTarget = (int) nearest;
    return ((float) currentTarget - detected) * p.amount + p.transpose;
}

void LiveTuner::pushHistory (float detected, float corrected)
{
    const int w = historyWrite.load (std::memory_order_relaxed);
    historyIn[(size_t) w].store (detected, std::memory_order_relaxed);
    historyOut[(size_t) w].store (corrected, std::memory_order_relaxed);
    historyWrite.store ((w + 1) % historySize, std::memory_order_release);
}

// ------------------------------------------------------------------ high quality (PSOLA)
void LiveTuner::placeMarks()
{
    const long long known = (nextFrame - 1) * hop; // latest time with pitch information
    for (;;)
    {
        const long long t = marks.back().pos;
        if (t > known)
            return;
        const double frame = (double) t / hop;
        const bool v = frameVoiced ((long long) std::lround (frame));
        long long next;
        if (v)
        {
            const double p = sr / midiToHz (frameMidi (frame));
            const long long lo = lastMarkVoiced ? (long long) (t + 0.8 * p) : t + 1;
            const long long hi = lastMarkVoiced ? (long long) (t + 1.2 * p) : (long long) (t + p);
            if (hi >= written)
                return;
            next = lo;
            for (long long i = lo; i <= hi; ++i)
                if (lp[(size_t) (i & ringMask)] > lp[(size_t) (next & ringMask)]) next = i;
            next = std::max (next, t + std::max (2ll, (long long) (0.5 * p)));
        }
        else
        {
            next = t + unvoicedStep;
            if (next >= written)
                return;
        }
        const bool nv = frameVoiced (std::min ((long long) std::lround ((double) next / hop), nextFrame - 1));
        marks.push_back ({ next, nv });
        lastMarkVoiced = v && nv;
    }
}

void LiveTuner::synthesise (const Params& p, int numCh)
{
    for (;;)
    {
        while (marks.size() > 2 && (double) marks[1].pos <= outMark)
            marks.pop_front();
        if (marks.size() < 3)
            break; // need the nearest mark and its successor
        size_t pick = 0;
        if (marks.size() > 1 && (double) marks[1].pos - outMark < outMark - (double) marks[0].pos)
            pick = 1;

        const long long c = marks[pick].pos;
        const int right = (int) (marks[pick + 1].pos - c);
        const int left = pick > 0 ? (int) (c - marks[pick - 1].pos) : right;
        const bool voiced = marks[pick].voiced;

        const double frame = outMark / hop;
        const float detected = frameMidi (frame);
        float ratio = 1.0f;
        if (voiced && detected > 0.0f)
        {
            const float desired = targetShift (detected, p);
            const double dt = (double) right / sr;
            const float a = p.retuneMs <= 0.5f ? 1.0f : (float) (1.0 - std::exp (-dt / (p.retuneMs * 0.001)));
            smoothShift += (desired - smoothShift) * a;
            ratio = std::exp2 (smoothShift / 12.0f);
            if (++historyDecim >= 2)
            {
                historyDecim = 0;
                pushHistory (detected, p.bypass ? 0.0f : detected + smoothShift);
            }
        }
        else if (! voiced)
        {
            // let go of the correction in breaths/consonants (~40 ms) so a breath that is briefly mistaken
            // for a note never inherits the previous note's full correction
            currentTarget = -1000;
            smoothShift *= (float) std::exp (-((double) right / sr) / 0.04);
        }

        const float formantRatio = std::max (0.75f, std::exp2 (p.formant / 12.0f));
        if (std::llround (outMark) - (long long) std::ceil (left / formantRatio) <= lastOutput)
            lateGrains.fetch_add (1, std::memory_order_relaxed);
        const float gain = voiced ? psolaLoudnessCompensation (ratio) : 1.0f;
        for (int ch = 0; ch < numCh; ++ch)
        {
            const auto& src = inCh[(size_t) ch];
            auto sample = [&src] (double pos)
            {
                const long long i = (long long) std::floor (pos);
                const float fr = (float) (pos - (double) i);
                if (i < 0) return 0.0f;
                return src[(size_t) (i & ringMask)] + (src[(size_t) ((i + 1) & ringMask)] - src[(size_t) (i & ringMask)]) * fr;
            };
            // the window sum is identical for every channel, so only the first one accumulates it
            const GrainTarget target { accCh[(size_t) ch].data(), ch == 0 ? wsum.data() : nullptr, ringSize, ringMask };
            addGrain (target, sample, (double) c, left, right, (long long) std::llround (outMark), formantRatio, gain);
        }
        outMark += std::max (1.0, (double) right / ratio);
    }
}

// ------------------------------------------------------------------ low latency (variable delay line)
// The output is read from the input through a delay that drifts at (1 - ratio) samples per sample, which
// resamples (pitch-shifts) the voice. Whenever the delay leaves a one-period window around `latencyLow`
// it jumps by exactly one detected pitch period with a short cross-fade; because a voice is periodic, the
// two taps line up and the jump is inaudible.
void LiveTuner::renderLowLatency (float* const* out, int numCh, int n, long long blockStart, const Params& p)
{
    const long long lastFrame = nextFrame - 1;
    const bool voiced = frameVoiced (lastFrame);
    const float detected = voiced ? frames[(size_t) (lastFrame % frameRing)] : 0.0f;
    float desired = 0.0f;
    float alpha;
    if (voiced)
    {
        desired = targetShift (detected, p);
        lastPeriod = sr / midiToHz (detected);
        alpha = p.retuneMs <= 0.5f ? 1.0f : (float) (1.0 - std::exp (-1.0 / (p.retuneMs * 0.001 * sr)));
    }
    else
    {
        currentTarget = -1000;
        alpha = (float) (1.0 - std::exp (-1.0 / (0.04 * sr))); // relax to no shift over ~40 ms in consonants/silence
    }
    const double centre = (double) latencyLow;
    const double period = std::clamp (lastPeriod, 16.0, (double) maxPeriod);
    const int fade = std::max (8, std::min ((int) period, (int) (0.004 * sr)));

    for (int i = 0; i < n; ++i)
    {
        llShift += (desired - llShift) * alpha;
        const double step = 1.0 - std::exp2 (llShift / 12.0);
        delay += step;
        if (fadeLen > 0)
            oldDelay += step;

        if (fadeLen == 0 && voiced)
        {
            if (delay < centre - 0.5 * period) { oldDelay = delay; delay += period; fadeLen = fade; fadePos = 0; }
            else if (delay > centre + 0.5 * period) { oldDelay = delay; delay -= period; fadeLen = fade; fadePos = 0; }
        }
        delay = std::clamp (delay, 2.0, (double) maxPeriod + centre);

        const long long t = blockStart + i;
        mixNow += ((p.bypass ? 0.0f : p.mix) - mixNow) * mixAlpha;
        float w = 1.0f;
        if (fadeLen > 0)
            w = 0.5f - 0.5f * std::cos (3.14159265f * (float) (fadePos + 1) / (float) fadeLen);
        for (int ch = 0; ch < numCh; ++ch)
        {
            float wet = readAt (ch, (double) t - delay);
            if (fadeLen > 0)
                wet = readAt (ch, (double) t - oldDelay) * (1.0f - w) + wet * w;
            const float dry = readAt (ch, (double) (t - latencyLow));
            out[ch][i] = dry + (wet - dry) * mixNow;
        }
        if (fadeLen > 0 && ++fadePos >= fadeLen)
            fadeLen = 0;
    }

    if (voiced && ++historyDecim >= 2)
    {
        historyDecim = 0;
        pushHistory (detected, p.bypass ? 0.0f : detected + llShift);
    }
}

// ------------------------------------------------------------------ main entry
void LiveTuner::process (const float* const* input, float* const* output, int numCh, int n, const Params& p)
{
    numCh = std::clamp (numCh, 1, 2);
    const long long blockStart = written;
    const float norm = 1.0f / (float) numCh;
    for (int i = 0; i < n; ++i)
    {
        const long long w = written + i;
        float m = 0.0f;
        for (int ch = 0; ch < numCh; ++ch)
        {
            const float x = input[ch][i];
            inCh[(size_t) ch][(size_t) (w & ringMask)] = x;
            m += x;
        }
        m *= norm;
        in[(size_t) (w & ringMask)] = m;
        lp1 = m + lpAlpha * (lp1 - m);
        lp2 = lp1 + lpAlpha * (lp2 - lp1);
        lp[(size_t) (w & ringMask)] = lp2;
    }
    written += n;

    analyseFrames();

    if (p.lowLatency)
    {
        // keep the PSOLA chain in step so switching engines later starts cleanly
        marks.clear();
        marks.push_back ({ written, false });
        lastMarkVoiced = false;
        outMark = (double) written;
        renderLowLatency (output, numCh, n, blockStart, p);
        return;
    }

    placeMarks();
    synthesise (p, numCh);

    lastOutput = blockStart + n - 1 - latencyHigh;
    for (int i = 0; i < n; ++i)
    {
        const long long t = blockStart + i - latencyHigh;
        const auto idx = (size_t) (t & ringMask);
        mixNow += ((p.bypass ? 0.0f : p.mix) - mixNow) * mixAlpha;
        for (int ch = 0; ch < numCh; ++ch)
        {
            float wet = 0.0f, dry = 0.0f;
            if (t >= 0)
            {
                wet = accCh[(size_t) ch][idx] / std::max (wsum[idx], 1.0f);
                dry = inCh[(size_t) ch][idx];
                accCh[(size_t) ch][idx] = 0.0f;
            }
            output[ch][i] = dry + (wet - dry) * mixNow;
        }
        if (t >= 0)
        {
            wsum[idx] = 0.0f;
            accCh[1][idx] = 0.0f;
        }
    }
}
} // namespace nf
