#include "NoteModel.h"
#include <algorithm>
#include <cmath>

namespace nf
{
float Analysis::midiAtFrame (double frame) const
{
    if (midi.empty())
        return 0.0f;
    const int k0 = std::clamp ((int) std::floor (frame), 0, numFrames() - 1);
    const int k1 = std::min (k0 + 1, numFrames() - 1);
    const float t = (float) std::clamp (frame - k0, 0.0, 1.0);
    const bool v0 = voiced[(size_t) k0] != 0, v1 = voiced[(size_t) k1] != 0;
    if (v0 && v1)
        return midi[(size_t) k0] + (midi[(size_t) k1] - midi[(size_t) k0]) * t;
    if (v0) return midi[(size_t) k0];
    if (v1) return midi[(size_t) k1];
    return 0.0f;
}

// ---------------------------------------------------------------- TimeMap
void TimeMap::build (const std::vector<Note>& notes, int hop, int numSamples)
{
    in.clear();
    out.clear();
    auto push = [this] (double i, double o)
    {
        if (! in.empty())
        {
            i = std::max (i, in.back());
            o = std::max (o, out.back());
            if (i == in.back() && o == out.back())
                return;
        }
        in.push_back (i);
        out.push_back (o);
    };
    push (0.0, 0.0);
    for (auto& n : notes)
    {
        push (n.inStart (hop), n.outStart);
        push (n.inEnd (hop), n.outEnd);
    }
    push ((double) numSamples, (double) numSamples);
}

static double mapPiecewise (const std::vector<double>& from, const std::vector<double>& to, double x)
{
    if (from.size() < 2)
        return x;
    if (x <= from.front())
        return to.front() + (x - from.front());
    if (x >= from.back())
        return to.back() + (x - from.back());
    auto it = std::upper_bound (from.begin(), from.end(), x);
    size_t j = (size_t) (it - from.begin()) - 1;
    // skip zero-width source segments
    while (j + 1 < from.size() && from[j + 1] == from[j])
        ++j;
    if (j + 1 >= from.size())
        return to.back();
    const double span = from[j + 1] - from[j];
    return to[j] + (x - from[j]) * (to[j + 1] - to[j]) / span;
}

double TimeMap::toOutput (double s) const { return mapPiecewise (in, out, s); }
double TimeMap::toInput (double s) const { return mapPiecewise (out, in, s); }

// ---------------------------------------------------------------- pitch decomposition
float computeCenter (const Analysis& a, int s, int e)
{
    const int len = e - s;
    const int trim = len >= 10 ? len / 10 : 0;
    double sum = 0.0, wsum = 0.0;
    for (int k = s + trim; k < e - trim; ++k)
    {
        if (! a.voicedAtFrame (k))
            continue;
        const double w = a.rms[(size_t) k] + 1.0e-6;
        sum += a.midi[(size_t) k] * w;
        wsum += w;
    }
    if (wsum <= 0.0)
    {
        for (int k = s; k < e; ++k)
            if (a.voicedAtFrame (k)) { sum += a.midi[(size_t) k]; wsum += 1.0; }
    }
    return wsum > 0.0 ? (float) (sum / wsum) : 60.0f;
}

std::vector<float> slowPitch (const Analysis& a, const Note& n)
{
    const int len = n.endFrame - n.startFrame;
    std::vector<float> out ((size_t) std::max (0, len), n.origCenter);
    // window of ~180 ms removes typical 4-8 Hz vibrato; it shrinks at note edges so glides stay "drift"
    const int radius = std::max (1, (int) std::lround (0.09 * a.sampleRate / a.hop));
    for (int i = 0; i < len; ++i)
    {
        const int k = n.startFrame + i;
        const int r = std::min ({ radius, i, len - 1 - i });
        double sum = 0.0;
        int cnt = 0;
        for (int j = k - r; j <= k + r; ++j)
            if (a.voicedAtFrame (j)) { sum += a.midi[(size_t) j]; ++cnt; }
        out[(size_t) i] = cnt > 0 ? (float) (sum / cnt) : (a.voicedAtFrame (k) ? a.midi[(size_t) k] : n.origCenter);
    }
    return out;
}

float editedPitch (const Analysis& a, const Note& n, const std::vector<float>& slow, int frame)
{
    if (! a.voicedAtFrame (frame) || frame < n.startFrame || frame >= n.endFrame)
        return 0.0f;
    const float p = a.midi[(size_t) frame];
    const float lp = slow[(size_t) (frame - n.startFrame)];
    return n.center() + n.driftScale * (lp - n.origCenter) + n.modScale * (p - lp);
}

RenderCurves buildCurves (const Analysis& a, const std::vector<Note>& notes)
{
    const int nf = a.numFrames();
    RenderCurves c;
    c.shift.assign ((size_t) nf, 0.0f);
    c.formant.assign ((size_t) nf, 0.0f);
    c.gain.assign ((size_t) nf, 1.0f);
    std::vector<int> owner ((size_t) nf, -1);

    // Raw per-note values, kept so boundaries can be cross-faded.
    std::vector<std::vector<float>> noteShift (notes.size());
    for (size_t i = 0; i < notes.size(); ++i)
    {
        const auto& n = notes[i];
        const auto slow = slowPitch (a, n);
        auto& sh = noteShift[i];
        sh.resize ((size_t) std::max (0, n.endFrame - n.startFrame));
        for (int k = n.startFrame; k < n.endFrame; ++k)
        {
            const float ep = editedPitch (a, n, slow, k);
            const float v = ep > 0.0f ? ep - a.midi[(size_t) k] : n.pitchOffset;
            sh[(size_t) (k - n.startFrame)] = v;
            if (k >= 0 && k < nf)
            {
                c.shift[(size_t) k] = v;
                c.formant[(size_t) k] = n.formant;
                c.gain[(size_t) k] = n.muted ? 0.0f : std::pow (10.0f, n.gainDb / 20.0f);
                owner[(size_t) k] = (int) i;
            }
        }
    }

    auto valueOf = [&] (int noteIdx, int k, int which) -> float
    {
        if (noteIdx < 0)
            return which == 2 ? 1.0f : 0.0f;
        const auto& n = notes[(size_t) noteIdx];
        if (which == 1) return n.formant;
        if (which == 2) return n.muted ? 0.0f : std::pow (10.0f, n.gainDb / 20.0f);
        const auto& sh = noteShift[(size_t) noteIdx];
        if (sh.empty()) return n.pitchOffset;
        return sh[(size_t) std::clamp (k - n.startFrame, 0, (int) sh.size() - 1)];
    };

    const int fade = std::max (2, (int) std::lround (0.022 * a.sampleRate / a.hop));
    auto cs = c; // read from raw values, write smoothed
    for (int b = 1; b < nf; ++b)
    {
        const int L = owner[(size_t) b - 1], R = owner[(size_t) b];
        if (L == R)
            continue;
        for (int k = std::max (0, b - fade); k < std::min (nf, b + fade); ++k)
        {
            const float t = (float) (k - (b - fade) + 0.5f) / (float) (2 * fade);
            const float w = t * t * (3.0f - 2.0f * t);
            for (int which = 0; which < 3; ++which)
            {
                auto& dst = which == 0 ? cs.shift : which == 1 ? cs.formant : cs.gain;
                dst[(size_t) k] = (1.0f - w) * valueOf (L, k, which) + w * valueOf (R, k, which);
            }
        }
    }
    return cs;
}

// ---------------------------------------------------------------- editing
double minNoteSamples (const Analysis& a) { return std::max (0.03 * a.sampleRate, 3.0 * a.hop); }

static bool touching (const Note& left, const Note& right, int hop)
{
    return std::abs (left.outEnd - right.outStart) < 0.5 * hop;
}

void moveNotesInTime (std::vector<Note>& notes, const std::vector<Note>& init, double delta, const Analysis& a)
{
    // Neighbours may be squeezed down to minLen; touching neighbours also stretch to follow.
    const double minLen = minNoteSamples (a);
    double lo = -1.0e18, hi = 1.0e18;
    const int count = (int) init.size();
    for (int i = 0; i < count; ++i)
    {
        if (! init[(size_t) i].selected)
            continue;
        const auto& n = init[(size_t) i];
        if (i == 0) lo = std::max (lo, -n.outStart);
        else if (! init[(size_t) i - 1].selected)
            lo = std::max (lo, (init[(size_t) i - 1].outStart + minLen) - n.outStart);
        if (i == count - 1) hi = std::min (hi, (double) a.numSamples - n.outEnd);
        else if (! init[(size_t) i + 1].selected)
            hi = std::min (hi, (init[(size_t) i + 1].outEnd - minLen) - n.outEnd);
    }
    if (lo > hi)
        return;
    delta = std::clamp (delta, lo, hi);

    notes = init;
    for (int i = 0; i < count; ++i)
    {
        if (! init[(size_t) i].selected)
            continue;
        auto& n = notes[(size_t) i];
        n.outStart += delta;
        n.outEnd += delta;
        if (i > 0 && ! init[(size_t) i - 1].selected)
        {
            auto& p = notes[(size_t) i - 1];
            if (touching (init[(size_t) i - 1], init[(size_t) i], a.hop) || p.outEnd > n.outStart)
                p.outEnd = n.outStart;
        }
        if (i + 1 < count && ! init[(size_t) i + 1].selected)
        {
            auto& q = notes[(size_t) i + 1];
            if (touching (init[(size_t) i], init[(size_t) i + 1], a.hop) || q.outStart < n.outEnd)
                q.outStart = n.outEnd;
        }
    }
}

void stretchNoteEdge (std::vector<Note>& notes, const std::vector<Note>& init, int idx, bool leftEdge,
                      double delta, const Analysis& a)
{
    const double minLen = minNoteSamples (a);
    notes = init;
    const int count = (int) init.size();
    const auto& n = init[(size_t) idx];
    if (leftEdge)
    {
        double v = n.outStart + delta;
        v = std::min (v, n.outEnd - minLen);
        bool follow = false;
        if (idx > 0)
        {
            const auto& p = init[(size_t) idx - 1];
            v = std::max (v, p.outStart + minLen);
            follow = touching (p, n, a.hop) || v < p.outEnd;
        }
        v = std::max (v, 0.0);
        notes[(size_t) idx].outStart = v;
        if (follow)
            notes[(size_t) idx - 1].outEnd = v;
    }
    else
    {
        double v = n.outEnd + delta;
        v = std::max (v, n.outStart + minLen);
        bool follow = false;
        if (idx + 1 < count)
        {
            const auto& q = init[(size_t) idx + 1];
            v = std::min (v, q.outEnd - minLen);
            follow = touching (n, q, a.hop) || v > q.outStart;
        }
        v = std::min (v, (double) a.numSamples);
        notes[(size_t) idx].outEnd = v;
        if (follow)
            notes[(size_t) idx + 1].outStart = v;
    }
}

bool splitNote (std::vector<Note>& notes, int idx, int frame, const Analysis& a)
{
    if (idx < 0 || idx >= (int) notes.size())
        return false;
    const Note n = notes[(size_t) idx];
    if (frame - n.startFrame < 3 || n.endFrame - frame < 3)
        return false;

    const double inLen = n.inEnd (a.hop) - n.inStart (a.hop);
    const double outK = n.outStart + ((double) frame * a.hop - n.inStart (a.hop)) * (n.outEnd - n.outStart) / inLen;

    Note left = n, right = n;
    left.endFrame = frame;
    left.outEnd = outK;
    right.startFrame = frame;
    right.outStart = outK;
    left.origCenter = computeCenter (a, left.startFrame, left.endFrame);
    right.origCenter = computeCenter (a, right.startFrame, right.endFrame);
    left.selected = right.selected = true;

    notes[(size_t) idx] = left;
    notes.insert (notes.begin() + idx + 1, right);
    return true;
}

bool mergeWithNext (std::vector<Note>& notes, int idx, const Analysis& a)
{
    if (idx < 0 || idx + 1 >= (int) notes.size())
        return false;
    auto& l = notes[(size_t) idx];
    const auto& r = notes[(size_t) idx + 1];
    if (l.endFrame != r.startFrame)
        return false;
    const float displayed = l.center();
    l.endFrame = r.endFrame;
    l.outEnd = r.outEnd;
    l.origCenter = computeCenter (a, l.startFrame, l.endFrame);
    l.pitchOffset = displayed - l.origCenter;
    notes.erase (notes.begin() + idx + 1);
    return true;
}

void correctPitch (std::vector<Note>& notes, float centerAmount, float driftAmount, const Scale* scale)
{
    for (auto& n : notes)
    {
        if (! n.selected)
            continue;
        const float cur = n.center();
        const float target = scale ? scale->snap (cur) : std::round (cur);
        n.pitchOffset += (target - cur) * centerAmount;
        n.driftScale *= (1.0f - driftAmount);
    }
}

void quantizeTime (std::vector<Note>& notes, double grid, double gridOffset, float amount, const Analysis& a)
{
    if (grid <= 0.0)
        return;
    for (size_t i = 0; i < notes.size(); ++i)
    {
        if (! notes[i].selected)
            continue;
        const double t = notes[i].outStart + gridOffset;
        const double target = std::round (t / grid) * grid - gridOffset;
        const double delta = (target - notes[i].outStart) * amount;
        auto init = notes;
        for (size_t j = 0; j < init.size(); ++j)
            init[j].selected = (j == i);
        auto moved = init;
        moveNotesInTime (moved, init, delta, a);
        for (size_t j = 0; j < moved.size(); ++j)
            moved[j].selected = notes[j].selected;
        notes = moved;
    }
}

void resetNote (Note& n)
{
    n.pitchOffset = 0.0f;
    n.modScale = 1.0f;
    n.driftScale = 1.0f;
    n.formant = 0.0f;
    n.gainDb = 0.0f;
    n.muted = false;
}

void resetTiming (std::vector<Note>& notes, int hop)
{
    for (auto& n : notes)
    {
        n.outStart = n.inStart (hop);
        n.outEnd = n.inEnd (hop);
    }
}
} // namespace nf
