#include "Scale.h"
#include <algorithm>
#include <cmath>
#include <limits>

namespace nf
{
const std::vector<ScaleDef>& allScales()
{
    static const std::vector<ScaleDef> scales {
        { "Chromatic",        { 1,1,1,1,1,1,1,1,1,1,1,1 } },
        { "Major",            { 1,0,1,0,1,1,0,1,0,1,0,1 } },
        { "Natural Minor",    { 1,0,1,1,0,1,0,1,1,0,1,0 } },
        { "Harmonic Minor",   { 1,0,1,1,0,1,0,1,1,0,0,1 } },
        { "Melodic Minor",    { 1,0,1,1,0,1,0,1,0,1,0,1 } },
        { "Major Pentatonic", { 1,0,1,0,1,0,0,1,0,1,0,0 } },
        { "Minor Pentatonic", { 1,0,0,1,0,1,0,1,0,0,1,0 } },
        { "Blues",            { 1,0,0,1,0,1,1,1,0,0,1,0 } },
        { "Dorian",           { 1,0,1,1,0,1,0,1,0,1,1,0 } },
        { "Phrygian",         { 1,1,0,1,0,1,0,1,1,0,1,0 } },
        { "Lydian",           { 1,0,1,0,1,0,1,1,0,1,0,1 } },
        { "Mixolydian",       { 1,0,1,0,1,1,0,1,0,1,1,0 } },
        { "Locrian",          { 1,1,0,1,0,1,1,0,1,0,1,0 } },
    };
    return scales;
}

std::vector<std::string> keyNames()
{
    return { "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B" };
}

unsigned presetMask (int root, int type)
{
    const auto& scales = allScales();
    const auto& def = scales[(size_t) std::clamp (type, 0, (int) scales.size() - 1)];
    unsigned m = 0;
    for (int d = 0; d < 12; ++d)
        if (def.degrees[(size_t) d])
            m |= 1u << ((root + d) % 12);
    return m;
}

bool Scale::contains (int midiNote) const
{
    const unsigned m = (mask & allNotesMask) == 0 ? allNotesMask : mask;
    return (m >> (((midiNote % 12) + 12) % 12)) & 1u;
}

float Scale::snap (float midi) const
{
    const int base = (int) std::floor (midi);
    float best = (float) std::lround (midi);
    float bestDist = std::numeric_limits<float>::max();
    for (int n = base - 6; n <= base + 7; ++n)
    {
        if (! contains (n))
            continue;
        const float dist = std::abs ((float) n - midi);
        if (dist < bestDist)
        {
            bestDist = dist;
            best = (float) n;
        }
    }
    return best;
}

int Scale::step (int midiNote, int direction) const
{
    int n = midiNote;
    for (int i = 0; i < 12; ++i)
    {
        n += direction > 0 ? 1 : -1;
        if (contains (n))
            return n;
    }
    return midiNote + direction;
}

std::string midiToName (float midi)
{
    const int n = (int) std::lround (midi);
    const auto names = keyNames();
    return names[(size_t) ((n % 12 + 12) % 12)] + std::to_string (n / 12 - 1);
}

float hzToMidi (float hz) { return 69.0f + 12.0f * std::log2 (hz / 440.0f); }
float midiToHz (float midi) { return 440.0f * std::exp2 ((midi - 69.0f) / 12.0f); }

Scale estimateKey (const std::vector<std::pair<float, float>>& pitchWeights)
{
    static const double major[12] = { 6.35, 2.23, 3.48, 2.33, 4.38, 4.09, 2.52, 5.19, 2.39, 3.66, 2.29, 2.88 };
    static const double minor[12] = { 6.33, 2.68, 3.52, 5.38, 2.60, 3.53, 2.54, 4.75, 3.98, 2.69, 3.34, 3.17 };

    double hist[12] = {};
    for (auto& [p, w] : pitchWeights)
        hist[(((int) std::lround (p)) % 12 + 12) % 12] += w;

    auto corr = [&] (const double* profile, int root)
    {
        double mx = 0, my = 0;
        for (int i = 0; i < 12; ++i) { mx += hist[(i + root) % 12]; my += profile[i]; }
        mx /= 12; my /= 12;
        double sxy = 0, sxx = 0, syy = 0;
        for (int i = 0; i < 12; ++i)
        {
            const double a = hist[(i + root) % 12] - mx, b = profile[i] - my;
            sxy += a * b; sxx += a * a; syy += b * b;
        }
        return sxx > 0 && syy > 0 ? sxy / std::sqrt (sxx * syy) : -1.0;
    };

    Scale best = Scale::fromPreset (0, 1);
    double bestR = -2.0;
    for (int root = 0; root < 12; ++root)
    {
        if (const double r = corr (major, root); r > bestR) { bestR = r; best = Scale::fromPreset (root, 1); }
        if (const double r = corr (minor, root); r > bestR) { bestR = r; best = Scale::fromPreset (root, 2); }
    }
    return best;
}
} // namespace nf
