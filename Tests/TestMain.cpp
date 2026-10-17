// Offline verification of the NoteForge DSP: builds a synthetic sung phrase with known pitches,
// runs analysis / edits / rendering, and re-measures the result.
#include <juce_audio_formats/juce_audio_formats.h>
#include <juce_gui_basics/juce_gui_basics.h>
#include "Engine.h"
#include "dsp/Align.h"
#include "dsp/LiveTuner.h"
#include "dsp/PianoSynth.h"
#include "dsp/Renderer.h"
#if NOTEFORGE_UI_SNAPSHOT
 #include "ui/NoteEditor.h"
 #include "PluginProcessor.h"
 #include "ui/FlBridge.h"
#endif
#include <cstdio>
#include <functional>
#include <random>

using namespace nf;

// ---- allocation tracking: any heap allocation made while gTrackAllocs is set on this thread is counted.
// The audio callback must never allocate (it can block and cause crackles/dropouts in the host).
static thread_local bool gTrackAllocs = false;
static std::atomic<int> gAudioThreadAllocs { 0 };
void* operator new (std::size_t n)
{
    if (gTrackAllocs) ++gAudioThreadAllocs;
    if (void* p = std::malloc (n ? n : 1)) return p;
    throw std::bad_alloc();
}
void* operator new[] (std::size_t n)
{
    if (gTrackAllocs) ++gAudioThreadAllocs;
    if (void* p = std::malloc (n ? n : 1)) return p;
    throw std::bad_alloc();
}
void operator delete (void* p) noexcept { std::free (p); }
void operator delete[] (void* p) noexcept { std::free (p); }
void operator delete (void* p, std::size_t) noexcept { std::free (p); }
void operator delete[] (void* p, std::size_t) noexcept { std::free (p); }

static int failures = 0;
static void check (bool ok, const char* what, double value)
{
    std::printf ("  [%s] %-58s %.4f\n", ok ? "PASS" : "FAIL", what, value);
    if (! ok) ++failures;
}

struct SynthNote { double start, dur; float midi; float vibDepth, vibRate; bool glideFromPrev; };

// Additive glottal-like source through a vowel formant filter.
static std::vector<float> synthPhrase (double sr, const std::vector<SynthNote>& notes, double total, float formantScale = 1.0f)
{
    const int n = (int) (total * sr);
    std::vector<float> src ((size_t) n, 0.0f);
    std::vector<float> f0 ((size_t) n, 0.0f), amp ((size_t) n, 0.0f);
    for (size_t i = 0; i < notes.size(); ++i)
    {
        const auto& nt = notes[i];
        const int s = (int) (nt.start * sr), e = (int) ((nt.start + nt.dur) * sr);
        for (int k = s; k < e && k < n; ++k)
        {
            const double t = (k - s) / sr;
            double m = nt.midi + nt.vibDepth * std::sin (2.0 * juce::MathConstants<double>::pi * nt.vibRate * t);
            if (nt.glideFromPrev && i > 0 && t < 0.06)
                m = notes[i - 1].midi + (m - notes[i - 1].midi) * (t / 0.06);
            f0[(size_t) k] = (float) (440.0 * std::pow (2.0, (m - 69.0) / 12.0));
            const double att = nt.glideFromPrev ? 1.0 : juce::jmin (1.0, t / 0.02);
            const double rel = juce::jmin (1.0, (nt.dur - t) / 0.03);
            amp[(size_t) k] = (float) juce::jmax (0.0, att * rel);
        }
    }
    double phase = 0.0;
    for (int k = 0; k < n; ++k)
    {
        if (f0[(size_t) k] <= 0.0f) { phase = 0.0; continue; }
        phase += f0[(size_t) k] / sr;
        phase -= std::floor (phase);
        const int harmonics = juce::jmin (60, (int) (7000.0 / f0[(size_t) k]));
        double v = 0.0;
        for (int h = 1; h <= harmonics; ++h)
            v += std::sin (2.0 * juce::MathConstants<double>::pi * h * phase) / std::pow ((double) h, 1.1);
        src[(size_t) k] = (float) (v * amp[(size_t) k]);
    }

    // cascade of three resonators: vowel /a/
    const double formants[3] = { 700.0 * formantScale, 1220.0 * formantScale, 2600.0 * formantScale };
    const double bws[3] = { 90.0, 110.0, 160.0 };
    std::vector<float> y = src;
    for (int f = 0; f < 3; ++f)
    {
        const double r = std::exp (-juce::MathConstants<double>::pi * bws[f] / sr);
        const double c = 2.0 * r * std::cos (2.0 * juce::MathConstants<double>::pi * formants[f] / sr);
        double y1 = 0, y2 = 0;
        const double g = (1.0 - r) * std::sqrt (1.0 - c + r * r);
        for (int k = 0; k < n; ++k)
        {
            const double v = g * y[(size_t) k] + c * y1 - r * r * y2;
            y2 = y1; y1 = v;
            y[(size_t) k] = (float) v;
        }
    }
    float peak = 0.0f;
    for (float v : y) peak = juce::jmax (peak, std::abs (v));
    for (auto& v : y) v *= 0.5f / peak;
    return y;
}

// A much more realistic sung voice than synthPhrase: glottal pulses with period jitter and amplitude
// shimmer, breath noise, irregular vibrato, scoops into notes, changing vowels, consonants and a little room.
struct RealNote { double start, dur; float midi; bool legato; int vowelFrom, vowelTo; };
static std::vector<float> synthRealVoice (double sr, const std::vector<RealNote>& notes, double total, uint32_t seed)
{
    const int n = (int) (total * sr);
    std::mt19937 rng (seed);
    std::normal_distribution<float> gauss (0.0f, 1.0f);
    std::uniform_real_distribution<float> uni (-1.0f, 1.0f);
    static const double vowels[3][5] = { { 700, 1220, 2600, 3300, 3750 },   // a
                                         { 300, 2300, 3000, 3500, 4000 },   // i
                                         { 330, 870, 2240, 3200, 3700 } };  // u
    const double bw[5] = { 80, 100, 140, 180, 220 };

    std::vector<float> f0 ((size_t) n, 0.0f), amp ((size_t) n, 0.0f), vow ((size_t) n, 0.0f);
    std::vector<int> vA ((size_t) n, 0), vB ((size_t) n, 0);
    for (size_t ni = 0; ni < notes.size(); ++ni)
    {
        const auto& nt = notes[ni];
        const int s = (int) (nt.start * sr), e = (int) ((nt.start + nt.dur) * sr);
        double vibPhase = uni (rng) * 3.0, vibRate = 5.2 + uni (rng) * 0.6;
        for (int k = s; k < e && k < n; ++k)
        {
            const double t = (k - s) / sr;
            vibRate += uni (rng) * 0.0005;
            vibPhase += 2.0 * juce::MathConstants<double>::pi * vibRate / sr;
            const double vibDepth = juce::jmin (1.0, t / 0.25) * (0.35 + 0.1 * std::sin (t * 1.7));
            double m = nt.midi + vibDepth * std::sin (vibPhase);
            if (nt.legato && ni > 0 && t < 0.07) m = notes[ni - 1].midi + (m - notes[ni - 1].midi) * (t / 0.07);
            else if (! nt.legato && t < 0.09) m -= 0.8 * (1.0 - t / 0.09); // scoop up into the note
            m += 0.12 * std::sin (t * 2.1 + ni);                          // slow drift
            f0[(size_t) k] = (float) (440.0 * std::pow (2.0, (m - 69.0) / 12.0));
            const double att = nt.legato ? 1.0 : juce::jmin (1.0, t / 0.03);
            const double rel = juce::jmin (1.0, (nt.dur - t) / 0.05);
            amp[(size_t) k] = (float) (juce::jmax (0.0, att * rel) * (0.85 + 0.15 * std::sin (t * 3.0)));
            vow[(size_t) k] = (float) juce::jlimit (0.0, 1.0, t / nt.dur);
            vA[(size_t) k] = nt.vowelFrom;
            vB[(size_t) k] = nt.vowelTo;
        }
        // consonant before non-legato notes ("s"/"t")
        if (! nt.legato && s > (int) (0.06 * sr))
            for (int k = s - (int) (0.05 * sr); k < s; ++k) amp[(size_t) k] = juce::jmax (amp[(size_t) k], 0.0f);
    }

    // glottal source (Rosenberg pulse derivative) with jitter + shimmer, plus breath noise
    std::vector<float> src ((size_t) n, 0.0f);
    double phase = 0.0, periodScale = 1.0;
    float shimmer = 1.0f, prevG = 0.0f;
    for (int k = 0; k < n; ++k)
    {
        if (f0[(size_t) k] <= 0.0f) { phase = 0.0; prevG = 0.0f; continue; }
        phase += f0[(size_t) k] / sr / periodScale;
        if (phase >= 1.0)
        {
            phase -= 1.0;
            periodScale = 1.0 + 0.009 * gauss (rng); // ~0.9 % jitter
            shimmer = 1.0f + 0.06f * gauss (rng);     // ~6 % shimmer
        }
        const double t = phase;
        double g = 0.0;
        if (t < 0.40) g = 0.5 * (1.0 - std::cos (juce::MathConstants<double>::pi * t / 0.40));
        else if (t < 0.58) g = std::cos (juce::MathConstants<double>::pi * (t - 0.40) / (2.0 * 0.18));
        const float dg = (float) g - prevG;
        prevG = (float) g;
        const float breath = 0.035f * gauss (rng) * (t < 0.58 ? 1.0f : 0.4f);
        src[(size_t) k] = (dg * 12.0f * shimmer + breath) * amp[(size_t) k];
    }
    // consonant noise
    for (auto& nt : notes)
        if (! nt.legato)
            for (int k = juce::jmax (0, (int) ((nt.start - 0.06) * sr)); k < (int) ((nt.start - 0.005) * sr); ++k)
                src[(size_t) k] += 0.05f * uni (rng);

    // time-varying vowel: 5 resonators in cascade, coefficients updated every 64 samples
    std::vector<float> y = src;
    double z1[5] = {}, z2[5] = {};
    double c1[5] = {}, c2[5] = {}, gn[5] = {};
    for (int k = 0; k < n; ++k)
    {
        if ((k & 63) == 0)
        {
            const float w = vow[(size_t) k];
            for (int f = 0; f < 5; ++f)
            {
                const double fr = vowels[vA[(size_t) k]][f] * (1.0 - w) + vowels[vB[(size_t) k]][f] * w;
                const double r = std::exp (-juce::MathConstants<double>::pi * bw[f] / sr);
                c1[f] = 2.0 * r * std::cos (2.0 * juce::MathConstants<double>::pi * fr / sr);
                c2[f] = -r * r;
                gn[f] = (1.0 - r) * std::sqrt (1.0 - c1[f] + r * r);
            }
        }
        double v = y[(size_t) k];
        for (int f = 0; f < 5; ++f)
        {
            const double o = gn[f] * v + c1[f] * z1[f] + c2[f] * z2[f];
            z2[f] = z1[f]; z1[f] = o; v = o;
        }
        y[(size_t) k] = (float) v;
    }
    // small room: two early reflections
    std::vector<float> room = y;
    const int d1 = (int) (0.011 * sr), d2 = (int) (0.023 * sr);
    for (int k = 0; k < n; ++k)
        room[(size_t) k] += 0.18f * (k >= d1 ? y[(size_t) (k - d1)] : 0.0f) + 0.1f * (k >= d2 ? y[(size_t) (k - d2)] : 0.0f);
    float peak = 0.0f;
    for (float v : room) peak = juce::jmax (peak, std::abs (v));
    for (auto& v : room) v *= 0.6f / peak;
    return room;
}

// "Glitchiness" of a rendered vocal compared with its source, measured inside note interiors.
struct GlitchStats { double roughness = 0, pitchJumps = 0, levelJumps = 0, maxLevelJumpDb = 0; };
static GlitchStats glitchStats (const std::vector<float>& y, double sr, const std::vector<std::pair<double, double>>& spans)
{
    const auto a = analyzeTake (y.data(), (int) y.size(), sr).analysis;
    GlitchStats g;
    int frames = 0;
    for (auto& [t0, t1] : spans)
    {
        const int k0 = (int) (t0 * sr / a.hop), k1 = (int) (t1 * sr / a.hop);
        for (int k = k0 + 1; k < k1 && k < a.numFrames(); ++k)
        {
            ++frames;
            g.roughness += a.aperiodicity[(size_t) k];
            if (! a.voiced[(size_t) k] || ! a.voiced[(size_t) k - 1] || std::abs (a.midi[(size_t) k] - a.midi[(size_t) k - 1]) > 0.5f)
                g.pitchJumps += 1;
            const double db = 20.0 * std::log10 ((a.rms[(size_t) k] + 1e-6) / (a.rms[(size_t) k - 1] + 1e-6));
            if (std::abs (db) > 2.0) g.levelJumps += 1;
            g.maxLevelJumpDb = juce::jmax (g.maxLevelJumpDb, std::abs (db));
        }
    }
    if (frames > 0) { g.roughness /= frames; g.pitchJumps *= 100.0 / frames; g.levelJumps *= 100.0 / frames; }
    return g;
}

// Artifacts the tuner adds to breaths/noise: frames that were unpitched noise in the input but come out
// pitched/buzzy, and frames whose high-frequency energy ("static", crackle) jumps well above the input's.
struct ArtifactStats { int breathFrames = 0, buzzInBreath = 0, staticFrames = 0, frames = 0; std::vector<double> worst; };
static ArtifactStats artifactStats (const std::vector<float>& in, const std::vector<float>& out, double sr)
{
    ArtifactStats s;
    const auto ai = analyzeTake (in.data(), (int) in.size(), sr).analysis;
    const auto ao = analyzeTake (out.data(), (int) out.size(), sr).analysis;
    const int hop = ai.hop;
    auto hf = [hop] (const std::vector<float>& v, int k)
    {
        double e = 0; int c = 0;
        for (int i = k * hop - hop; i < k * hop + hop; ++i)
            if (i > 0 && i < (int) v.size()) { const double d = v[(size_t) i] - v[(size_t) i - 1]; e += d * d; ++c; }
        return c ? std::sqrt (e / c) : 0.0;
    };
    const float breathGate = std::pow (10.0f, -58.0f / 20.0f);
    for (int k = 2; k < juce::jmin (ai.numFrames(), ao.numFrames()); ++k)
    {
        ++s.frames;
        const bool breath = ! ai.voiced[(size_t) k] && ai.rms[(size_t) k] > breathGate && ai.aperiodicity[(size_t) k] > 0.3f;
        if (breath)
        {
            ++s.breathFrames;
            if (ao.aperiodicity[(size_t) k] < 0.15f && ao.rms[(size_t) k] > breathGate) ++s.buzzInBreath;
        }
        const double hi = hf (in, k), ho = hf (out, k);
        if (ho > 2.0 * hi + 2e-4)
        {
            ++s.staticFrames;
            if (s.worst.size() < 12) s.worst.push_back (k * hop / sr);
        }
    }
    return s;
}

// Clicks: samples whose second difference is far above the local (5 ms) level of second differences.
static int countClicks (const std::vector<float>& y, double sr, double t0, double t1)
{
    const int w = (int) (0.005 * sr);
    std::vector<float> d2 (y.size(), 0.0f);
    for (size_t i = 2; i < y.size(); ++i) d2[i] = std::abs (y[i] - 2.0f * y[i - 1] + y[i - 2]);
    int clicks = 0;
    for (int i = (int) (t0 * sr) + w; i < (int) (t1 * sr) - w && i < (int) y.size() - w; i += 1)
    {
        double local = 0;
        for (int j = i - w; j < i + w; j += 4) local += d2[(size_t) j];
        local /= (2 * w / 4);
        if (d2[(size_t) i] > 10.0 * local + 1e-4) { ++clicks; i += w; }
    }
    return clicks;
}

static void writeWav (const juce::File& f, const std::vector<float>& x, double sr)
{
    f.deleteFile();
    juce::WavAudioFormat fmt;
    std::unique_ptr<juce::OutputStream> os = std::make_unique<juce::FileOutputStream> (f);
    auto w = fmt.createWriterFor (os, juce::AudioFormatWriterOptions{}.withSampleRate (sr).withNumChannels (1).withBitsPerSample (24));
    if (w == nullptr) return;
    juce::AudioBuffer<float> b (1, (int) x.size());
    for (int i = 0; i < (int) x.size(); ++i) b.setSample (0, i, x[(size_t) i]);
    w->writeFromAudioSampleBuffer (b, 0, b.getNumSamples());
}

// Weighted mean pitch and stddev of voiced frames whose centre lies in [t0, t1] seconds.
struct PitchStats { float mean = 0, sd = 0; int frames = 0; };
static PitchStats pitchIn (const Analysis& a, double t0, double t1)
{
    double s = 0, ss = 0; int c = 0;
    for (int k = 0; k < a.numFrames(); ++k)
    {
        const double t = (double) k * a.hop / a.sampleRate;
        if (t < t0 || t > t1 || ! a.voiced[(size_t) k]) continue;
        s += a.midi[(size_t) k]; ss += a.midi[(size_t) k] * a.midi[(size_t) k]; ++c;
    }
    PitchStats p;
    p.frames = c;
    if (c > 0) { p.mean = (float) (s / c); p.sd = (float) std::sqrt (juce::jmax (0.0, ss / c - p.mean * p.mean)); }
    return p;
}

static double rmsIn (const std::vector<float>& x, double sr, double t0, double t1)
{
    double s = 0; int c = 0;
    for (int i = (int) (t0 * sr); i < (int) (t1 * sr) && i < (int) x.size(); ++i) { s += x[(size_t) i] * x[(size_t) i]; ++c; }
    return c ? std::sqrt (s / c) : 0.0;
}

static double centroidIn (const std::vector<float>& x, double sr, double t0, double t1)
{
    FFT fft (12);
    const int N = fft.size();
    double num = 0, den = 0;
    for (int s = (int) (t0 * sr); s + N < (int) (t1 * sr); s += N / 2)
    {
        std::vector<std::complex<float>> b ((size_t) N);
        for (int i = 0; i < N; ++i)
            b[(size_t) i] = { x[(size_t) (s + i)] * (0.5f - 0.5f * std::cos (6.2831853f * i / N)), 0.0f };
        fft.forward (b.data());
        for (int k = 1; k < N / 2; ++k)
        {
            const double m = std::abs (b[(size_t) k]);
            num += m * k * sr / N; den += m;
        }
    }
    return den > 0 ? num / den : 0.0;
}

static int noteAt (const std::vector<Note>& notes, const Analysis& a, double t)
{
    for (size_t i = 0; i < notes.size(); ++i)
        if (t * a.sampleRate >= notes[i].outStart && t * a.sampleRate < notes[i].outEnd) return (int) i;
    return -1;
}

static std::vector<float> renderWith (const std::vector<float>& x, const Analysis& a, const std::vector<Note>& n)
{
    std::vector<float> out;
    renderTake (x.data(), a, n, out);
    return out;
}

static std::vector<std::vector<float>> readAudioFile (const juce::File& f, double& rate)
{
    juce::AudioFormatManager fm;
    fm.registerBasicFormats();
    std::unique_ptr<juce::AudioFormatReader> r (fm.createReaderFor (f));
    std::vector<std::vector<float>> out;
    if (r == nullptr) return out;
    rate = r->sampleRate;
    juce::AudioBuffer<float> b ((int) r->numChannels, (int) r->lengthInSamples);
    r->read (&b, 0, b.getNumSamples(), 0, true, true);
    for (int ch = 0; ch < b.getNumChannels(); ++ch)
        out.emplace_back (b.getReadPointer (ch), b.getReadPointer (ch) + b.getNumSamples());
    std::printf ("  %s: %d ch, %.0f Hz, %d bit, %.1f s\n", f.getFileName().toRawUTF8(), (int) r->numChannels, r->sampleRate,
                 (int) r->bitsPerSample, b.getNumSamples() / r->sampleRate);
    return out;
}

static int realRecordingReport (const juce::File& wav, const juce::File& outDir);

int main()
{
    if (auto cmp = juce::SystemStats::getEnvironmentVariable ("NF_COMPARE", {}); cmp.isNotEmpty())
    {
        // NF_COMPARE=a.wav|b.wav : how different are two recordings (e.g. dry insert vs processed insert)?
        juce::ScopedJuceInitialiser_GUI gui;
        double ra = 0, rb = 0;
        auto a = readAudioFile (juce::File (cmp.upToFirstOccurrenceOf ("|", false, false)), ra);
        auto b = readAudioFile (juce::File (cmp.fromFirstOccurrenceOf ("|", false, false)), rb);
        if (a.empty() || b.empty()) return 1;
        double s = 0, e = 0;
        const size_t n = juce::jmin (a[0].size(), b[0].size());
        for (size_t i = 0; i < n; ++i) { s += a[0][i] * a[0][i]; e += std::pow (a[0][i] - b[0][i], 2); }
        std::printf ("  difference: %.1f dB below the signal\n", 10 * std::log10 ((s + 1e-20) / (e + 1e-20)));

        // line b up against a (b = a processed by the chain, with some latency)
        std::vector<float> ma (a[0].size()), mb (b[0].size());
        for (size_t i = 0; i < ma.size(); ++i) { ma[i] = 0; for (auto& ch : a) ma[i] += ch[i] / (float) a.size(); }
        for (size_t i = 0; i < mb.size(); ++i) { mb[i] = 0; for (auto& ch : b) mb[i] += ch[i] / (float) b.size(); }
        // pick the loudest 4 s of a as the probe
        const int win = (int) (4.0 * ra);
        int bestS = 0; double bestE = -1;
        for (int s0 = 0; s0 + win < (int) ma.size(); s0 += (int) ra / 2)
        {
            double en = 0; for (int i = s0; i < s0 + win; i += 4) en += ma[(size_t) i] * ma[(size_t) i];
            if (en > bestE) { bestE = en; bestS = s0; }
        }
        const auto al = findChunkInTake (mb.data(), (int) mb.size(), ma.data() + bestS, juce::jmin (win, (int) ma.size() - bestS));
        const long long lag = al.offset - bestS;
        std::printf ("  b is a delayed by %lld samples (%.1f ms), match %.0f%%\n", lag, 1000.0 * (double) lag / ra, al.confidence * 100.0);
        // static report on the aligned pair: a = before, b = after
        std::vector<float> bAligned (ma.size(), 0.0f);
        for (size_t i = 0; i < ma.size(); ++i) { const long long k = (long long) i + lag; if (k >= 0 && k < (long long) mb.size()) bAligned[i] = mb[(size_t) k]; }
        double xy = 0, xx = 0;
        for (size_t i = 0; i < ma.size(); ++i) { xy += ma[i] * bAligned[i]; xx += ma[i] * ma[i]; }
        std::printf ("  level of b relative to a: %.1f dB\n", 20 * std::log10 (std::abs (xy / (xx + 1e-20)) + 1e-12));
        const auto art = artifactStats (ma, bAligned, ra);
        std::printf ("  breath frames %d, breath turned buzzy %d, static/crackle frames %d of %d; clicks a %d, b %d\n", art.breathFrames,
                     art.buzzInBreath, art.staticFrames, art.frames, countClicks (ma, ra, 0.0, ma.size() / ra), countClicks (bAligned, ra, 0.0, ma.size() / ra));
        if (! art.worst.empty()) { std::printf ("  static at:"); for (double t : art.worst) std::printf (" %.2fs", t); std::printf ("\n"); }
        const auto od = juce::File::getCurrentWorkingDirectory().getChildFile ("test-out").getChildFile ("pair");
        od.createDirectory();
        writeWav (od.getChildFile ("a_before.wav"), ma, ra);
        writeWav (od.getChildFile ("b_after_aligned.wav"), bAligned, ra);
        return 0;
    }
    if (auto realPath = juce::SystemStats::getEnvironmentVariable ("NF_REAL_WAV", {}); realPath.isNotEmpty())
    {
        juce::ScopedJuceInitialiser_GUI gui;
        return realRecordingReport (juce::File (realPath), juce::File::getCurrentWorkingDirectory().getChildFile ("test-out").getChildFile ("real"));
    }

    juce::ScopedJuceInitialiser_GUI gui;
    const double sr = 44100.0;
    const auto outDir = juce::File::getCurrentWorkingDirectory().getChildFile ("test-out");
    outDir.createDirectory();

    const std::vector<SynthNote> phrase {
        { 0.10, 0.60, 57.0f, 0.45f, 5.5f, false }, // A3 with vibrato
        { 0.80, 0.50, 60.3f, 0.00f, 0.0f, false }, // C4, 30 cents sharp
        { 1.30, 0.50, 63.8f, 0.10f, 5.0f, true  }, // legato to E4, 20 cents flat
        { 1.95, 0.40, 67.0f, 0.00f, 0.0f, false }, // G4
        { 2.40, 0.35, 64.0f, 0.00f, 0.0f, false }, // E4
    };
    auto x = synthPhrase (sr, phrase, 3.0);
    // a sibilant between the first two notes
    std::mt19937 rng (7);
    std::normal_distribution<float> nd (0.0f, 0.02f);
    float hp = 0.0f, prev = 0.0f;
    for (int i = (int) (0.72 * sr); i < (int) (0.79 * sr); ++i) { const float w = nd (rng); hp = 0.9f * (hp + w - prev); prev = w; x[(size_t) i] += hp; }
    writeWav (outDir.getChildFile ("00_input.wav"), x, sr);

    std::printf ("== analysis\n");
    auto res = analyzeTake (x.data(), (int) x.size(), sr);
    const auto& a = res.analysis;
    auto notes = res.notes;
    for (auto& n : notes)
        std::printf ("  note %.3f-%.3fs centre %.2f (%s)\n", n.outStart / sr, n.outEnd / sr, n.origCenter, midiToName (n.origCenter).c_str());
    if (juce::SystemStats::getEnvironmentVariable ("NF_DUMP_FRAMES", {}).isNotEmpty())
        for (int k = 0; k < a.numFrames(); ++k)
            std::printf ("    f%4d %.4fs v%d midi %.2f rms %.4f\n", k, k * a.hop / sr, a.voiced[(size_t) k], a.midi[(size_t) k], a.rms[(size_t) k]);
    const float expected[] = { 57.0f, 60.3f, 63.8f, 67.0f, 64.0f };
    for (int i = 0; i < 5; ++i)
    {
        const double mid = phrase[(size_t) i].start + phrase[(size_t) i].dur * 0.5;
        const int idx = noteAt (notes, a, mid);
        const float c = idx >= 0 ? notes[(size_t) idx].origCenter : 0.0f;
        check (idx >= 0 && std::abs (c - expected[i]) < 0.15f, ("detected centre of synth note " + std::to_string (i)).c_str(), c);
    }
    check (notes.size() >= 5 && notes.size() <= 7, "note count within 5..7", (double) notes.size());

    std::printf ("== identity render\n");
    {
        auto y = renderWith (x, a, notes);
        double e = 0, s = 0;
        for (size_t i = 0; i < x.size(); ++i) { s += x[i] * x[i]; e += (x[i] - y[i]) * (x[i] - y[i]); }
        const double snr = 10.0 * std::log10 (s / juce::jmax (e, 1e-20));
        check (snr > 40.0, "unedited render SNR (dB) > 40", snr);
        writeWav (outDir.getChildFile ("01_identity.wav"), y, sr);
    }

    const int iA = noteAt (notes, a, 0.40), iC = noteAt (notes, a, 1.05), iE = noteAt (notes, a, 1.55), iG = noteAt (notes, a, 2.15);

    std::printf ("== pitch shift\n");
    for (float semis : { 3.0f, -5.0f, 12.0f, -12.0f })
    {
        auto e = notes;
        e[(size_t) iC].pitchOffset = semis;
        auto y = renderWith (x, a, e);
        auto ra = analyzeTake (y.data(), (int) y.size(), sr);
        const auto ps = pitchIn (ra.analysis, 0.9, 1.2);
        const double want = 60.3 + semis;
        check (std::abs (ps.mean - want) < 0.12, ("C4 note shifted by " + std::to_string ((int) semis) + " st: pitch error").c_str(), ps.mean - want);
        const double r0 = rmsIn (x, sr, 0.9, 1.2), r1 = rmsIn (y, sr, 0.9, 1.2);
        check (std::abs (20.0 * std::log10 (r1 / r0)) < 2.5, "  loudness change (dB) within 2.5", 20.0 * std::log10 (r1 / r0));
        if (semis == 3.0f) writeWav (outDir.getChildFile ("02_shift_up3.wav"), y, sr);
    }

    std::printf ("== correct pitch macro (snap centres to C major)\n");
    {
        auto e = notes;
        for (auto& n : e) n.selected = true;
        const Scale cmaj = Scale::fromPreset (0, 1);
        correctPitch (e, 1.0f, 0.0f, &cmaj);
        auto y = renderWith (x, a, e);
        auto ra = analyzeTake (y.data(), (int) y.size(), sr);
        const auto pc = pitchIn (ra.analysis, 0.9, 1.2), pe = pitchIn (ra.analysis, 1.45, 1.7);
        check (std::abs (pc.mean - 60.0f) < 0.1f, "C4 (+30c) corrected to 60.00", pc.mean);
        check (std::abs (pe.mean - 64.0f) < 0.1f, "E4 (-20c) corrected to 64.00", pe.mean);
        writeWav (outDir.getChildFile ("03_corrected.wav"), y, sr);
    }

    std::printf ("== vibrato (pitch modulation)\n");
    {
        const auto before = pitchIn (a, 0.25, 0.6);
        auto e = notes;
        e[(size_t) iA].modScale = 0.0f;
        auto y = renderWith (x, a, e);
        auto ra = analyzeTake (y.data(), (int) y.size(), sr);
        const auto after = pitchIn (ra.analysis, 0.25, 0.6);
        check (before.sd > 0.2f, "original vibrato depth (sd, st)", before.sd);
        check (after.sd < before.sd * 0.3f, "vibrato flattened (sd, st)", after.sd);
        check (std::abs (after.mean - 57.0f) < 0.1f, "  centre preserved", after.mean);
        writeWav (outDir.getChildFile ("04_no_vibrato.wav"), y, sr);

        e[(size_t) iA].modScale = 2.0f;
        auto y2 = renderWith (x, a, e);
        auto rb = analyzeTake (y2.data(), (int) y2.size(), sr);
        const auto doubled = pitchIn (rb.analysis, 0.25, 0.6);
        check (doubled.sd > before.sd * 1.6f, "vibrato doubled (sd, st)", doubled.sd);
        writeWav (outDir.getChildFile ("05_double_vibrato.wav"), y2, sr);
    }

    std::printf ("== formant\n");
    {
        auto e = notes;
        e[(size_t) iG].formant = 4.0f;
        auto y = renderWith (x, a, e);
        auto ra = analyzeTake (y.data(), (int) y.size(), sr);
        const double c0 = centroidIn (x, sr, 2.0, 2.3), c1 = centroidIn (y, sr, 2.0, 2.3);
        const auto pg = pitchIn (ra.analysis, 2.0, 2.3);
        check (c1 > c0 * 1.12, "spectral centroid raised by formant +4 (ratio)", c1 / c0);
        check (std::abs (pg.mean - 67.0f) < 0.12f, "  pitch unchanged", pg.mean);
        writeWav (outDir.getChildFile ("06_formant_up.wav"), y, sr);
    }

    std::printf ("== timing\n");
    {
        auto init = notes;
        for (auto& n : init) n.selected = false;
        auto e = init;
        stretchNoteEdge (e, init, iG, false, 0.15 * sr, a); // lengthen G4 by 150 ms (if room)
        const double gained = (e[(size_t) iG].outEnd - init[(size_t) iG].outEnd) / sr;
        auto y = renderWith (x, a, e);
        auto ra = analyzeTake (y.data(), (int) y.size(), sr);
        auto rn = ra.notes;
        const int g = noteAt (rn, ra.analysis, 2.15);
        const double len0 = (init[(size_t) iG].outEnd - init[(size_t) iG].outStart) / sr;
        const double len1 = g >= 0 ? (rn[(size_t) g].outEnd - rn[(size_t) g].outStart) / sr : 0.0;
        check (gained > 0.03, "edge moved (s)", gained);
        check (std::abs ((len1 - len0) - gained) < 0.04, "re-detected G4 lengthened by the same amount (s)", len1 - len0);
        const auto pg = pitchIn (ra.analysis, 2.0, 2.3);
        check (std::abs (pg.mean - 67.0f) < 0.12f, "  stretched note keeps its pitch", pg.mean);
        writeWav (outDir.getChildFile ("07_stretched.wav"), y, sr);

        auto m = init;
        m[(size_t) iE].selected = true;
        auto moved = m;
        moveNotesInTime (moved, m, -0.1 * sr, a);
        check (moved[(size_t) iE].outStart < m[(size_t) iE].outStart, "legato note moved earlier", (moved[(size_t) iE].outStart - m[(size_t) iE].outStart) / sr);
        check (std::abs (moved[(size_t) iE - 1].outEnd - moved[(size_t) iE].outStart) < 1.0, "touching neighbour follows", moved[(size_t) iE - 1].outEnd - moved[(size_t) iE].outStart);
    }

    std::printf ("== split / merge\n");
    {
        auto e = notes;
        const int frame = (notes[(size_t) iA].startFrame + notes[(size_t) iA].endFrame) / 2;
        check (splitNote (e, iA, frame, a), "split A3 in half", (double) e.size());
        check (mergeWithNext (e, iA, a), "merge it back", (double) e.size());
        check (e.size() == notes.size(), "  note count restored", (double) e.size());
    }

    std::printf ("== realistic voice: do edits sound glitchy?\n");
    {
        const std::vector<RealNote> song {
            { 0.10, 0.70, 57.25f, false, 0, 0 }, { 0.80, 0.45, 60.2f, true, 0, 1 }, { 1.25, 0.55, 64.3f, true, 1, 0 },
            { 1.95, 0.60, 62.1f, false, 2, 0 }, { 2.70, 0.80, 59.3f, false, 0, 2 }, { 3.50, 0.50, 52.2f, false, 0, 0 } };
        const auto rv = synthRealVoice (sr, song, 4.2, 99);
        writeWav (outDir.getChildFile ("20_real_voice_in.wav"), rv, sr);
        std::vector<std::pair<double, double>> spans;
        for (auto& s : song) spans.push_back ({ s.start + 0.12, s.start + s.dur - 0.08 });
        const auto ra = analyzeTake (rv.data(), (int) rv.size(), sr);
        std::printf ("  detected %d notes:", (int) ra.notes.size());
        for (auto& nn : ra.notes) std::printf (" %s", midiToName (nn.origCenter).c_str());
        std::printf ("\n");
        const auto base = glitchStats (rv, sr, spans);
        std::printf ("  %-28s roughness %.3f  pitch-jumps %5.1f%%  level-jumps %5.1f%%  max-step %4.1f dB\n", "original",
                     base.roughness, base.pitchJumps, base.levelJumps, base.maxLevelJumpDb);

        struct Case { const char* name; std::function<void (std::vector<Note>&)> edit; };
        const std::vector<Case> cases {
            { "no edit", [] (std::vector<Note>&) {} },
            { "all notes +2 st", [] (std::vector<Note>& v) { for (auto& n : v) n.pitchOffset += 2.0f; } },
            { "all notes -3 st", [] (std::vector<Note>& v) { for (auto& n : v) n.pitchOffset -= 3.0f; } },
            { "correct pitch 100%/50%", [] (std::vector<Note>& v) { for (auto& n : v) n.selected = true; correctPitch (v, 1.0f, 0.5f, nullptr); } },
            { "vibrato flattened", [] (std::vector<Note>& v) { for (auto& n : v) n.modScale = 0.0f; } },
            { "+1 st and formant +2", [] (std::vector<Note>& v) { for (auto& n : v) { n.pitchOffset += 1.0f; n.formant = 2.0f; } } },
        };
        int idx = 0;
        for (auto& cs : cases)
        {
            auto e = ra.notes;
            cs.edit (e);
            std::vector<float> y;
            renderTake (rv.data(), ra.analysis, e, y);
            const auto g = glitchStats (y, sr, spans);
            std::printf ("  %-28s roughness %.3f  pitch-jumps %5.1f%%  level-jumps %5.1f%%  max-step %4.1f dB\n", cs.name,
                         g.roughness, g.pitchJumps, g.levelJumps, g.maxLevelJumpDb);
            check (g.roughness < base.roughness + 0.03, ("  " + std::string (cs.name) + ": no added roughness").c_str(), g.roughness - base.roughness);
            check (g.pitchJumps < base.pitchJumps + 1.5, ("  " + std::string (cs.name) + ": no pitch glitches").c_str(), g.pitchJumps - base.pitchJumps);
            check (g.levelJumps < base.levelJumps + 1.5 && g.maxLevelJumpDb < base.maxLevelJumpDb + 2.0,
                   ("  " + std::string (cs.name) + ": no volume glitches").c_str(), g.levelJumps - base.levelJumps);
            const int clicksIn = countClicks (rv, sr, 0.0, 4.2), clicksOut = countClicks (y, sr, 0.0, 4.2);
            std::printf ("  %-28s clicks: original %d, edited %d\n", "", clicksIn, clicksOut);
            check (clicksOut <= clicksIn + 2, ("  " + std::string (cs.name) + ": no clicks").c_str(), clicksOut);
            writeWav (outDir.getChildFile ("21_real_voice_edit" + std::to_string (idx++) + ".wav"), y, sr);
        }
    }

    {
        // the click detector really does catch a click
        auto rv = synthRealVoice (sr, { { 0.10, 1.0, 57.0f, false, 0, 0 } }, 1.2, 5);
        const int before = countClicks (rv, sr, 0.0, 1.2);
        for (size_t i = (size_t) (0.5 * sr); i < (size_t) (0.5 * sr) + 64; ++i) rv[i] += 0.2f;
        check (countClicks (rv, sr, 0.0, 1.2) >= before + 1, "click detector sanity check (catches an injected click)", countClicks (rv, sr, 0.0, 1.2));
    }

    std::printf ("== realistic voice WITH REVERB before NoteForge\n");
    {
        const std::vector<RealNote> song {
            { 0.10, 0.70, 57.25f, false, 0, 0 }, { 0.80, 0.45, 60.2f, true, 0, 1 }, { 1.25, 0.55, 64.3f, true, 1, 0 },
            { 1.95, 0.60, 62.1f, false, 2, 0 }, { 2.70, 0.80, 59.3f, false, 0, 2 }, { 3.50, 0.50, 52.2f, false, 0, 0 } };
        auto rv = synthRealVoice (sr, song, 4.2, 99);
        juce::Reverb verb;
        juce::Reverb::Parameters vp;
        vp.roomSize = 0.75f; vp.damping = 0.4f; vp.wetLevel = 0.35f; vp.dryLevel = 0.8f; vp.width = 0.0f;
        verb.setParameters (vp);
        verb.setSampleRate (sr);
        verb.processMono (rv.data(), (int) rv.size());
        writeWav (outDir.getChildFile ("22_reverb_voice_in.wav"), rv, sr);
        std::vector<std::pair<double, double>> spans;
        for (auto& s : song) spans.push_back ({ s.start + 0.12, s.start + s.dur - 0.08 });
        const auto ra = analyzeTake (rv.data(), (int) rv.size(), sr);
        const auto base = glitchStats (rv, sr, spans);
        std::printf ("  %-28s roughness %.3f  pitch-jumps %5.1f%%  level-jumps %5.1f%%\n", "original (with reverb)", base.roughness, base.pitchJumps, base.levelJumps);
        auto e = ra.notes;
        for (auto& n : e) n.selected = true;
        correctPitch (e, 1.0f, 0.5f, nullptr);
        for (auto& n : e) n.pitchOffset += 2.0f;
        std::vector<float> y;
        renderTake (rv.data(), ra.analysis, e, y);
        const auto g = glitchStats (y, sr, spans);
        std::printf ("  %-28s roughness %.3f  pitch-jumps %5.1f%%  level-jumps %5.1f%%\n", "correct + 2 st", g.roughness, g.pitchJumps, g.levelJumps);
        writeWav (outDir.getChildFile ("23_reverb_voice_edited.wav"), y, sr);
    }

    std::printf ("== custom note set (note picker)\n");
    {
        Scale cTriad { 0, 0, (1u << 0) | (1u << 4) | (1u << 7) }; // only C, E, G allowed
        check (cTriad.snap (61.2f) == 60.0f && cTriad.snap (62.6f) == 64.0f && cTriad.snap (66.0f) == 67.0f,
               "snap goes only to the picked notes (C/E/G)", cTriad.snap (62.6f));
        check (Scale::fromPreset (9, 2).mask == presetMask (9, 2) && Scale::fromPreset (9, 2).contains (60)
                   && ! Scale::fromPreset (9, 2).contains (61), "A minor preset mask", (double) presetMask (9, 2));
        auto e = notes;
        for (auto& n : e) n.selected = true;
        correctPitch (e, 1.0f, 0.0f, &cTriad);
        check (std::abs (e[(size_t) iE].center() - 64.0f) < 1e-4f && std::abs (e[(size_t) iC].center() - 60.0f) < 1e-4f,
               "correct pitch uses the picked notes", e[(size_t) iE].center());
    }

    std::printf ("== live auto-tune (per voice range)\n");
    {
        std::vector<SynthNote> held { { 0.05, 1.4, 60.4f, 0.0f, 0.0f, false } }; // 40 cents sharp
        auto lx = synthPhrase (sr, held, 1.5);
        const auto pin = pitchIn (analyzeTake (lx.data(), (int) lx.size(), sr).analysis, 0.3, 1.2);
        check (std::abs (pin.mean - 60.4f) < 0.08f, "live input pitch", pin.mean);
        const float rangeHz[] = { 140.0f, 90.0f, 60.0f };
        const char* rangeName[] = { "high", "mid", "low" };
        for (int rIdx = 0; rIdx < 3; ++rIdx)
        {
            LiveTuner tuner;
            tuner.prepare (sr, rangeHz[rIdx]);
            LiveTuner::Params p;
            p.scale = Scale::fromPreset (0, 0);
            p.retuneMs = 0.0f;
            std::vector<float> ly (lx.size());
            for (size_t i = 0; i < lx.size(); i += 512)
            {
                const int len = (int) juce::jmin ((size_t) 512, lx.size() - i);
                tuner.process (lx.data() + i, ly.data() + i, len, p);
            }
            const int lat = tuner.latencySamples();
            std::vector<float> aligned (ly.begin() + lat, ly.end());
            auto ra = analyzeTake (aligned.data(), (int) aligned.size(), sr);
            const auto pout = pitchIn (ra.analysis, 0.3, 1.2);
            std::printf ("  %s range: latency %d samples (%.1f ms, was 118.9 ms)\n", rangeName[rIdx], lat, 1000.0 * lat / sr);
            check (std::abs (pout.mean - 60.0f) < 0.08f, "  live output snapped to C4", pout.mean);
            check (pout.sd < 0.08f, "  output steady (sd)", pout.sd);
            const double ldb = 20.0 * std::log10 (rmsIn (aligned, sr, 0.3, 1.2) / rmsIn (lx, sr, 0.3, 1.2));
            check (std::abs (ldb) < 2.0, "  live loudness change (dB) within 2", ldb);
            // note onsets line up once the reported latency is compensated
            auto onset = [&] (const std::vector<float>& v)
            {
                for (size_t i = 0; i < v.size(); ++i) if (std::abs (v[i]) > 0.1f) return (int) i;
                return -1;
            };
            check (std::abs (onset (aligned) - onset (lx)) < (int) (0.006 * sr), "  onset aligned after latency compensation (samples)", onset (aligned) - onset (lx));

            // with correction off, the output is the input delayed by exactly the reported latency
            LiveTuner bypass;
            bypass.prepare (sr, rangeHz[rIdx]);
            p.amount = 0.0f;
            std::vector<float> by (lx.size());
            for (size_t i = 0; i < lx.size(); i += 512)
                bypass.process (lx.data() + i, by.data() + i, (int) juce::jmin ((size_t) 512, lx.size() - i), p);
            double es = 0, ss = 0;
            for (int i = (int) (0.3 * sr); i < (int) (1.2 * sr); ++i)
            {
                const double d = by[(size_t) (i + lat)] - lx[(size_t) i];
                es += d * d; ss += (double) lx[(size_t) i] * lx[(size_t) i];
            }
            check (10 * std::log10 (ss / juce::jmax (es, 1e-20)) > 30.0, "  0% correction = input delayed by latency (SNR dB)", 10 * std::log10 (ss / juce::jmax (es, 1e-20)));
            if (rIdx == 1)
            {
                writeWav (outDir.getChildFile ("08_live_in.wav"), lx, sr);
                writeWav (outDir.getChildFile ("09_live_out.wav"), aligned, sr);
            }
        }
    }

    std::printf ("== live: low-latency engine (for recording)\n");
    {
        std::vector<SynthNote> held { { 0.05, 1.4, 60.4f, 0.0f, 0.0f, false } }; // 40 cents sharp
        const auto lx = synthPhrase (sr, held, 1.5);
        auto maxStep = [] (const std::vector<float>& v, size_t a, size_t b)
        {
            float m = 0.0f;
            for (size_t i = a + 1; i < b; ++i) m = juce::jmax (m, std::abs (v[i] - v[i - 1]));
            return m;
        };
        const float rangeHz[] = { 140.0f, 90.0f, 60.0f };
        const char* rangeName[] = { "high", "mid", "low/auto" };
        for (int rIdx = 0; rIdx < 3; ++rIdx)
        {
            LiveTuner tuner;
            tuner.prepare (sr, rangeHz[rIdx]);
            LiveTuner::Params p;
            p.scale = Scale::fromPreset (0, 0);
            p.retuneMs = 0.0f;
            p.lowLatency = true;
            std::vector<float> ly (lx.size());
            for (size_t i = 0; i < lx.size(); i += 256)
                tuner.process (lx.data() + i, ly.data() + i, (int) juce::jmin ((size_t) 256, lx.size() - i), p);
            const int lat = tuner.latencySamples (true);
            std::vector<float> aligned (ly.begin() + lat, ly.end());
            const auto pout = pitchIn (analyzeTake (aligned.data(), (int) aligned.size(), sr).analysis, 0.3, 1.2);
            std::printf ("  %s range: delay %.1f ms (high-quality engine: %.1f ms)\n", rangeName[rIdx], 1000.0 * lat / sr,
                         1000.0 * tuner.latencySamples (false) / sr);
            check (1000.0 * lat / sr < 10.0, "  delay under 10 ms", 1000.0 * lat / sr);
            check (std::abs (pout.mean - 60.0f) < 0.08f, "  output snapped to C4", pout.mean);
            check (pout.sd < 0.08f, "  output steady (sd)", pout.sd);
            const double ldb = 20.0 * std::log10 (rmsIn (aligned, sr, 0.3, 1.2) / rmsIn (lx, sr, 0.3, 1.2));
            check (std::abs (ldb) < 1.0, "  loudness unchanged (dB)", ldb);
            const float jIn = maxStep (lx, (size_t) (0.3 * sr), (size_t) (1.2 * sr));
            const float jOut = maxStep (aligned, (size_t) (0.3 * sr), (size_t) (1.2 * sr));
            check (jOut < jIn * 1.5f, "  no clicks at the period jumps (max step ratio)", jOut / jIn);

            LiveTuner bypass;
            bypass.prepare (sr, rangeHz[rIdx]);
            p.amount = 0.0f;
            std::vector<float> by (lx.size());
            for (size_t i = 0; i < lx.size(); i += 256)
                bypass.process (lx.data() + i, by.data() + i, (int) juce::jmin ((size_t) 256, lx.size() - i), p);
            double es = 0, ss = 0;
            for (int i = (int) (0.3 * sr); i < (int) (1.2 * sr); ++i)
            {
                const double d = by[(size_t) (i + lat)] - lx[(size_t) i];
                es += d * d; ss += (double) lx[(size_t) i] * lx[(size_t) i];
            }
            check (10 * std::log10 (ss / juce::jmax (es, 1e-20)) > 60.0, "  0% correction = input delayed by the reported delay (SNR dB)", 10 * std::log10 (ss / juce::jmax (es, 1e-20)));
            if (rIdx == 2)
                writeWav (outDir.getChildFile ("12_live_lowlatency_out.wav"), aligned, sr);
        }

        // stereo: both sides tuned, the stereo picture kept (no collapse to mono)
        std::vector<float> L = lx, R (lx.size());
        for (size_t i = 0; i < lx.size(); ++i) R[i] = 0.4f * lx[i] + 0.3f * lx[(i + 3000) % lx.size()];
        for (bool lowLat : { true, false })
        {
            LiveTuner tuner;
            tuner.prepare (sr, LiveTuner::autoRangeHz);
            LiveTuner::Params p;
            p.scale = Scale::fromPreset (0, 0);
            p.retuneMs = 0.0f;
            p.lowLatency = lowLat;
            std::vector<float> oL (lx.size()), oR (lx.size());
            for (size_t i = 0; i < lx.size(); i += 512)
            {
                const int len = (int) juce::jmin ((size_t) 512, lx.size() - i);
                const float* ins[] = { L.data() + i, R.data() + i };
                float* outs[] = { oL.data() + i, oR.data() + i };
                tuner.process (ins, outs, 2, len, p);
            }
            const int lat = tuner.latencySamples (lowLat);
            std::vector<float> aL (oL.begin() + lat, oL.end()), aR (oR.begin() + lat, oR.end());
            const auto pl = pitchIn (analyzeTake (aL.data(), (int) aL.size(), sr).analysis, 0.3, 1.2);
            double corr = 0, eL = 0, eR = 0;
            for (int i = (int) (0.3 * sr); i < (int) (1.2 * sr); ++i) { corr += aL[(size_t) i] * aR[(size_t) i]; eL += aL[(size_t) i] * aL[(size_t) i]; eR += aR[(size_t) i] * aR[(size_t) i]; }
            const double inCorr = [&] { double c = 0, a = 0, b = 0; for (int i = (int) (0.3 * sr); i < (int) (1.2 * sr); ++i) { c += L[(size_t) i] * R[(size_t) i]; a += L[(size_t) i] * L[(size_t) i]; b += R[(size_t) i] * R[(size_t) i]; } return c / std::sqrt (a * b); }();
            const char* label = lowLat ? "low latency" : "high quality";
            check (std::abs (pl.mean - 60.0f) < 0.08f, (std::string ("  stereo (") + label + "): left channel tuned").c_str(), pl.mean);
            check (std::abs (corr / std::sqrt (eL * eR) - inCorr) < 0.1, (std::string ("  stereo (") + label + "): L/R difference kept (correlation)").c_str(), corr / std::sqrt (eL * eR));
        }
    }

    std::printf ("== live: breath noise must not be mistaken for notes\n");
    {
        std::mt19937 rng (21);
        std::normal_distribution<float> gn (0.0f, 1.0f);
        auto resonate = [&] (std::vector<float> v, std::vector<std::pair<double, double>> fb)
        {
            for (auto [f, bw] : fb)
            {
                const double r = std::exp (-juce::MathConstants<double>::pi * bw / sr);
                const double c1 = 2.0 * r * std::cos (2.0 * juce::MathConstants<double>::pi * f / sr), c2 = -r * r;
                double z1 = 0, z2 = 0;
                for (auto& s : v) { const double o = (1.0 - r) * s + c1 * z1 + c2 * z2; z2 = z1; z1 = o; s = (float) o; }
            }
            float pk = 0; for (float s : v) pk = juce::jmax (pk, std::abs (s));
            for (auto& s : v) s *= 0.1f / pk;
            return v;
        };
        const int len = (int) (3.0 * sr);
        std::vector<float> white ((size_t) len);
        for (auto& s : white) s = gn (rng);
        const struct { const char* name; std::vector<float> sig; } breaths[] = {
            { "breath (soft noise)", resonate (white, { { 1500.0, 2500.0 } }) },
            { "breath through 'ah' mouth shape", resonate (white, { { 700.0, 130.0 }, { 1220.0, 150.0 }, { 2600.0, 200.0 } }) },
            { "breath with a whistle resonance", resonate (white, { { 900.0, 40.0 }, { 2400.0, 300.0 } }) },
        };
        for (auto& b : breaths)
            for (bool lowLat : { false, true })
            {
                LiveTuner tuner;
                tuner.prepare (sr, LiveTuner::autoRangeHz);
                LiveTuner::Params p;
                p.scale = Scale::fromPreset (0, 0);
                p.retuneMs = 140.0f;
                p.lowLatency = lowLat;
                std::vector<float> y (b.sig.size());
                for (size_t i = 0; i < b.sig.size(); i += 256)
                    tuner.process (b.sig.data() + i, y.data() + i, (int) juce::jmin ((size_t) 256, b.sig.size() - i), p);
                if (! lowLat)
                {
                    // distribution of one- vs two-period periodicity on this breath
                    PitchDetector det;
                    det.prepare (sr, LiveTuner::autoRangeHz);
                    double a1 = 0, a2 = 0; int c = 0, a2High = 0;
                    for (int s0 = 0; s0 + det.samplesNeeded() < (int) b.sig.size(); s0 += 256)
                    {
                        const auto r = det.analyze (b.sig.data() + s0);
                        if (r.aperiodicity < 0.2f && r.aperiodicity2 >= 0.0f) { a1 += r.aperiodicity; a2 += r.aperiodicity2; ++c; if (r.aperiodicity2 > 0.25f) ++a2High; }
                    }
                    if (c) std::printf ("    %s: 'pitched-looking' frames %d, mean 1-period %.3f, 2-period %.3f, 2-period > 0.25 in %.0f%%\n",
                                        b.name, c, a1 / c, a2 / c, 100.0 * a2High / c);
                    // frame-to-frame pitch wander of consecutive pitched-looking frames
                    float prevM = 0; int jc = 0; int jb[4] = {};
                    for (int s0 = 0; s0 + det.samplesNeeded() < (int) b.sig.size(); s0 += 256)
                    {
                        const auto r = det.analyze (b.sig.data() + s0);
                        const float mm = (r.aperiodicity < 0.3f && r.freqHz > 0) ? hzToMidi (r.freqHz) : 0.0f;
                        if (mm > 0 && prevM > 0) { const float d = std::abs (mm - prevM); ++jc; ++jb[d < 0.1f ? 0 : d < 0.2f ? 1 : d < 0.4f ? 2 : 3]; }
                        prevM = mm;
                    }
                    if (jc) std::printf ("      pitch wander per frame: <0.1st %.0f%%  <0.2 %.0f%%  <0.4 %.0f%%  >=0.4 %.0f%%\n",
                                         100.0 * jb[0] / jc, 100.0 * jb[1] / jc, 100.0 * jb[2] / jc, 100.0 * jb[3] / jc);
                }
                const double pct = 100.0 * tuner.voicedFrameCount.load() / juce::jmax (1, tuner.framesAnalysed.load());
                std::printf ("    %s%s: frames taken as a note %.1f%%, late grains %d\n", b.name, lowLat ? " [low latency]" : " [high quality]", pct, tuner.lateGrains.load());

                // what actually matters: sung note (needing a strong correction) -> this breath -> sung note.
                // The breath must come out the way it went in: no static, no buzz.
                auto song = synthRealVoice (sr, { { 0.05, 0.9, 60.45f, false, 0, 0 }, { 2.05, 0.9, 64.45f, false, 0, 0 } }, 3.0, 17);
                for (int i = 0; i < (int) (1.0 * sr); ++i)
                {
                    const float env = (float) std::sin (juce::MathConstants<double>::pi * i / sr);
                    song[(size_t) (0.97 * sr) + (size_t) i] += b.sig[(size_t) i] * 1.5f * env;
                }
                LiveTuner t2;
                t2.prepare (sr, LiveTuner::autoRangeHz);
                std::vector<float> o2 (song.size());
                for (size_t i = 0; i < song.size(); i += 256)
                    t2.process (song.data() + i, o2.data() + i, (int) juce::jmin ((size_t) 256, song.size() - i), p);
                const int lat = t2.latencySamples (lowLat);
                std::vector<float> al (o2.begin() + lat, o2.end());
                al.resize (song.size(), 0.0f);
                // high-frequency energy (first difference) inside the breath, out vs in
                double hi = 0, ho = 0;
                for (int i = (int) (1.1 * sr); i < (int) (1.85 * sr); ++i)
                {
                    hi += std::pow (song[(size_t) i] - song[(size_t) i - 1], 2);
                    ho += std::pow (al[(size_t) i] - al[(size_t) i - 1], 2);
                }
                const double staticDb = 10.0 * std::log10 ((ho + 1e-12) / (hi + 1e-12));
                check (std::abs (staticDb) < 1.5, (std::string ("    ") + b.name + (lowLat ? " [low latency]" : " [high quality]")
                                                   + ": breath after a corrected note unchanged (HF dB)").c_str(), staticDb);
                if (! lowLat && std::string (b.name).find ("whistle") != std::string::npos)
                {
                    writeWav (outDir.getChildFile ("32_note_breath_note_in.wav"), song, sr);
                    writeWav (outDir.getChildFile ("33_note_breath_note_hq_140ms.wav"), al, sr);
                }
            }
    }

    {
        // the same statistic on sung voices (should stay low, so the check never rejects real singing)
        for (float note : { 45.0f, 57.0f, 64.0f, 69.0f, 76.0f, 81.0f })
        {
            const auto voice = synthRealVoice (sr, { { 0.05, 1.2, note, false, 0, 2 } }, 1.3, 31);
            PitchDetector det;
            det.prepare (sr, LiveTuner::autoRangeHz);
            int c = 0, a2High = 0, noCheck = 0;
            double a2 = 0;
            for (int s0 = (int) (0.2 * sr); s0 + det.samplesNeeded() < (int) (1.1 * sr); s0 += 256)
            {
                const auto r = det.analyze (voice.data() + s0);
                if (r.aperiodicity >= 0.2f) continue;
                if (r.aperiodicity2 < 0.0f) { ++noCheck; continue; }
                a2 += r.aperiodicity2; ++c; if (r.aperiodicity2 > 0.25f) ++a2High;
            }
            std::printf ("    sung %s: frames %d, mean 2-period %.3f, 2-period > 0.25 in %.0f%%, too low to check %d\n",
                         midiToName (note).c_str(), c, c ? a2 / c : 0.0, c ? 100.0 * a2High / c : 0.0, noCheck);
        }
    }

    {
        // cost of one pitch-analysis frame (the expensive step that lands in a single callback)
        PitchDetector det;
        det.prepare (sr, LiveTuner::autoRangeHz);
        std::vector<float> buf ((size_t) det.samplesNeeded() + 1024);
        for (size_t i = 0; i < buf.size(); ++i) buf[i] = std::sin ((float) i * 0.03f) + 0.3f * std::sin ((float) i * 0.11f);
        double best = 1e9, total = 0;
        for (int r = 0; r < 400; ++r)
        {
            const auto t0 = juce::Time::getHighResolutionTicks();
            det.analyze (buf.data() + (r % 512));
            const double ms = juce::Time::highResolutionTicksToSeconds (juce::Time::getHighResolutionTicks() - t0) * 1000.0;
            best = juce::jmin (best, ms); total += ms;
        }
        std::printf ("== pitch analysis frame: typical %.3f ms, best %.3f ms\n", total / 400.0, best);
    }
    std::printf ("== live: CPU per audio callback (worst case must fit the buffer)\n");
    {
        std::vector<RealNote> phrase { { 0.1, 1.5, 57.3f, false, 0, 1 }, { 1.8, 1.5, 64.2f, false, 2, 0 }, { 3.5, 1.4, 52.4f, false, 0, 0 } };
        auto v = synthRealVoice (sr, phrase, 5.0, 23);
        for (bool lowLat : { false, true })
            for (int bs : { 32, 64, 128, 256, 512 })
            {
                LiveTuner tuner;
                tuner.prepare (sr, LiveTuner::autoRangeHz);
                LiveTuner::Params p;
                p.scale = Scale::fromPreset (0, 0);
                p.retuneMs = 140.0f;
                p.lowLatency = lowLat;
                std::vector<float> L (v), R (v), oL (v.size()), oR (v.size());
                double worst = 0, total = 0; int calls = 0;
                for (size_t i = 0; i + (size_t) bs <= v.size(); i += (size_t) bs)
                {
                    const float* ins[] = { L.data() + i, R.data() + i };
                    float* outs[] = { oL.data() + i, oR.data() + i };
                    const auto t0 = juce::Time::getHighResolutionTicks();
                    tuner.process (ins, outs, 2, bs, p);
                    const double ms = juce::Time::highResolutionTicksToSeconds (juce::Time::getHighResolutionTicks() - t0) * 1000.0;
                    if (i > 0) { worst = juce::jmax (worst, ms); total += ms; ++calls; }
                }
                const double budget = 1000.0 * bs / sr;
                std::printf ("  %-12s buffer %3d (%.2f ms): average %.3f ms, worst %.3f ms = %.0f%% of the buffer\n",
                             lowLat ? "low latency" : "high quality", bs, budget, total / calls, worst, 100.0 * worst / budget);
                if (bs >= 64)
                    check (worst < 0.5 * budget, "    worst callback under 50% of the buffer time", 100.0 * worst / budget);
            }
    }

    std::printf ("== live: bypass\n");
    {
        const auto v = synthRealVoice (sr, { { 0.05, 2.4, 60.4f, false, 0, 1 } }, 2.6, 13); // 40 cents sharp
        for (bool lowLat : { false, true })
        {
            const char* eng = lowLat ? "low latency" : "high quality";
            // 1) fully bypassed: you hear exactly your voice, at the same delay
            LiveTuner tuner;
            tuner.prepare (sr, LiveTuner::autoRangeHz);
            LiveTuner::Params p;
            p.scale = Scale::fromPreset (0, 0);
            p.retuneMs = 0.0f;
            p.lowLatency = lowLat;
            p.bypass = true;
            std::vector<float> y (v.size());
            for (size_t i = 0; i < v.size(); i += 256)
                tuner.process (v.data() + i, y.data() + i, (int) juce::jmin ((size_t) 256, v.size() - i), p);
            const int lat = tuner.latencySamples (lowLat);
            double e = 0, s = 0;
            for (size_t i = (size_t) (0.2 * sr); i + (size_t) lat < v.size(); ++i) { s += v[i] * v[i]; e += std::pow (y[i + (size_t) lat] - v[i], 2); }
            check (10 * std::log10 (s / juce::jmax (e, 1e-20)) > 60.0, (std::string ("  ") + eng + ": bypassed output = your voice at the same delay (SNR dB)").c_str(), 10 * std::log10 (s / juce::jmax (e, 1e-20)));
            int inPts = 0, outPts = 0;
            for (int k = 0; k < LiveTuner::historySize; ++k)
            {
                if (tuner.historyIn[(size_t) k].load() > 0.0f) ++inPts;
                if (tuner.historyOut[(size_t) k].load() > 0.0f) ++outPts;
            }
            check (inPts > 50 && outPts == 0, (std::string ("  ") + eng + ": graph gets grey (sung) points but no orange ones").c_str(), outPts);

            // 2) toggled in the middle of a held note: tuned -> bypass -> tuned, no clicks
            LiveTuner t2;
            t2.prepare (sr, LiveTuner::autoRangeHz);
            p.bypass = false;
            std::vector<float> z (v.size());
            for (size_t i = 0; i < v.size(); i += 256)
            {
                p.bypass = (i > (size_t) (0.9 * sr) && i < (size_t) (1.6 * sr));
                t2.process (v.data() + i, z.data() + i, (int) juce::jmin ((size_t) 256, v.size() - i), p);
            }
            float maxJump = 0, ref = 0;
            for (size_t i = (size_t) (0.5 * sr); i < (size_t) (2.2 * sr); ++i)
            {
                maxJump = juce::jmax (maxJump, std::abs (z[i] - z[i - 1]));
                ref = juce::jmax (ref, std::abs (v[i] - v[i - 1]));
            }
            check (maxJump < ref * 1.3f, (std::string ("  ") + eng + ": switching bypass on and off mid-note doesn't click (jump ratio)").c_str(), maxJump / ref);
            if (! lowLat) writeWav (outDir.getChildFile ("36_bypass_toggle_hq.wav"), z, sr);
        }
    }

    std::printf ("== live: host pauses processing during a breath (FL smart disable)\n");
    {
        const std::vector<RealNote> lines { { 0.1, 1.2, 57.3f, false, 0, 1 }, { 1.9, 1.3, 60.4f, false, 2, 0 } };
        const auto v = synthRealVoice (sr, lines, 3.5, 41);
        for (bool lowLat : { false, true })
            for (bool resetOnGap : { false, true })
            {
                LiveTuner tuner;
                tuner.prepare (sr, LiveTuner::autoRangeHz);
                LiveTuner::Params p;
                p.scale = Scale::fromPreset (0, 0);
                p.retuneMs = 46.0f;
                p.lowLatency = lowLat;
                std::vector<float> out;
                const size_t gapStart = (size_t) (1.45 * sr), gapEnd = (size_t) (1.75 * sr); // host stops calling us in the breath
                std::vector<float> blk (256);
                for (size_t i = 0; i + 256 <= v.size(); i += 256)
                {
                    if (i >= gapStart && i < gapEnd) continue; // no callbacks at all during the gap
                    if (i >= gapEnd && i < gapEnd + 256 && resetOnGap) tuner.resetAfterGap();
                    tuner.process (v.data() + i, blk.data(), 256, p);
                    out.insert (out.end(), blk.begin(), blk.end());
                }
                // the first sung line after the gap must not start with clicks/static
                const int lat = tuner.latencySamples (lowLat);
                const size_t resume = gapStart; // where post-gap audio starts in `out`
                double maxJump = 0, ref = 0;
                for (size_t i = resume + 1; i < juce::jmin (out.size(), resume + (size_t) lat + (size_t) (0.25 * sr)); ++i)
                    maxJump = juce::jmax (maxJump, (double) std::abs (out[i] - out[i - 1]));
                for (size_t i = (size_t) (2.2 * sr) + 1; i < (size_t) (2.6 * sr); ++i)
                    ref = juce::jmax (ref, (double) std::abs (v[i] - v[i - 1]));
                const std::string name = std::string (lowLat ? "low latency" : "high quality") + (resetOnGap ? ", with gap reset" : ", no gap handling");
                std::printf ("  %s: biggest sample jump after resuming %.4f (normal singing: %.4f)\n", name.c_str(), maxJump, ref);
                if (resetOnGap)
                    check (maxJump < ref * 1.5, ("  " + name + ": clean restart after the gap").c_str(), maxJump / ref);
                if (! lowLat)
                    writeWav (outDir.getChildFile (resetOnGap ? "35_gap_reset_hq.wav" : "34_gap_no_reset_hq.wav"), out, sr);
            }
    }

    std::printf ("== live: a song with low AND high notes\n");
    {
        // E2 and A2 (low male voice) then E4 and A4, all sung 30 cents sharp
        const std::vector<SynthNote> wide { { 0.05, 0.55, 40.3f, 0.0f, 0.0f, false }, { 0.70, 0.55, 45.3f, 0.0f, 0.0f, false },
                                            { 1.35, 0.55, 64.3f, 0.0f, 0.0f, false }, { 2.00, 0.55, 69.3f, 0.0f, 0.0f, false } };
        const auto wx = synthPhrase (sr, wide, 2.7);
        auto runLive = [&] (float lowestHz, LiveTuner& tuner, bool lowLat)
        {
            tuner.prepare (sr, lowestHz);
            LiveTuner::Params p;
            p.scale = Scale::fromPreset (0, 0);
            p.retuneMs = 0.0f;
            p.lowLatency = lowLat;
            std::vector<float> y (wx.size());
            for (size_t i = 0; i < wx.size(); i += 512)
                tuner.process (wx.data() + i, y.data() + i, (int) juce::jmin ((size_t) 512, wx.size() - i), p);
            return std::vector<float> (y.begin() + tuner.latencySamples (lowLat), y.end());
        };
        const float targets[] = { 40.0f, 45.0f, 64.0f, 69.0f };
        const struct { const char* name; float hz; bool expectAllTuned; bool lowLat; } cases[] = {
            { "Mid", 90.0f, false, false }, { "Auto", LiveTuner::autoRangeHz, true, false },
            { "Auto + recording engine", LiveTuner::autoRangeHz, true, true } };
        for (auto& cs : cases)
        {
            LiveTuner tuner;
            const auto out = runLive (cs.hz, tuner, cs.lowLat);
            const auto ra = analyzeTake (out.data(), (int) out.size(), sr);
            int tuned = 0;
            std::printf ("  %s (%.0f ms):", cs.name, 1000.0 * tuner.latencySamples (cs.lowLat) / sr);
            for (int k = 0; k < 4; ++k)
            {
                const auto ps = pitchIn (ra.analysis, wide[(size_t) k].start + 0.15, wide[(size_t) k].start + 0.45);
                std::printf ("  %s %+.0fc", midiToName (targets[k]).c_str(), (ps.mean - targets[k]) * 100.0f);
                if (std::abs (ps.mean - targets[k]) < 0.08f) ++tuned;
            }
            std::printf ("\n");
            if (cs.expectAllTuned)
                check (tuned == 4, "  Auto range tunes the low and the high notes (count)", tuned);
            else
                check (tuner.notesBelowRange.load() > 0, "  Mid range reports notes below its range", tuner.notesBelowRange.load());
        }
    }

    std::printf ("== partial re-render matches a full render\n");
    {
        Engine eng;
        eng.loadTakeBlocking (x, sr, 0);
        eng.renderBlocking();
        auto en = eng.getNotes();
        en[(size_t) iC].pitchOffset = 3.0f;
        en[(size_t) iC].modScale = 0.0f;
        eng.setNotes (en);
        eng.renderBlocking();
        const auto renderedRange = eng.getLastRenderedRange();
        check (renderedRange.getLength() < (int) x.size(), "edit re-rendered only part of the take (s)", renderedRange.getLength() / sr);
        const auto partial = eng.getRenderedCopy()[0];
        std::vector<float> full;
        renderTake (x.data(), *eng.getAnalysis(), en, full);
        auto snr = [] (const std::vector<float>& ref, const std::vector<float>& got, size_t from, size_t to)
        {
            double e = 0, s = 0;
            for (size_t i = from; i < to; ++i) { s += ref[i] * ref[i]; e += (ref[i] - got[i]) * (ref[i] - got[i]); }
            return 10 * std::log10 (s / juce::jmax (e, 1e-20));
        };
        check (snr (full, partial, 0, full.size()) > 90.0, "partial result identical to full render (SNR dB)", snr (full, partial, 0, full.size()));

        // a second edit elsewhere, then revert the first: still identical
        en[(size_t) iG].pitchOffset = -2.0f;
        eng.setNotes (en);
        eng.renderBlocking();
        en[(size_t) iC].pitchOffset = 0.0f;
        en[(size_t) iC].modScale = 1.0f;
        eng.setNotes (en);
        eng.renderBlocking();
        renderTake (x.data(), *eng.getAnalysis(), en, full);
        const auto p2 = eng.getRenderedCopy()[0];
        check (snr (full, p2, 0, full.size()) > 90.0, "after several partial edits (SNR dB)", snr (full, p2, 0, full.size()));
        const size_t a0 = (size_t) (0.12 * sr), a1 = (size_t) (0.65 * sr);
        check (snr (x, p2, a0, a1) > 60.0, "untouched A3 unchanged (SNR dB)", snr (x, p2, a0, a1));
        const size_t c0s = (size_t) (0.85 * sr), c1s = (size_t) (1.25 * sr);
        check (snr (x, p2, c0s, c1s) > 40.0, "reverted C4 back to original (SNR dB)", snr (x, p2, c0s, c1s));
    }

    std::printf ("== stereo take\n");
    {
        std::vector<std::vector<float>> st (2, x);
        for (size_t i = 0; i < x.size(); ++i) st[1][i] = 0.5f * x[i] + 0.02f * x[(i + 37) % x.size()];
        Engine eng;
        eng.loadTakeBlocking (st, sr, 0);
        auto en = eng.getNotes();
        en[(size_t) iC].pitchOffset = 4.0f;
        eng.setNotes (en);
        eng.renderBlocking();
        const auto out = eng.getRenderedCopy();
        check (out.size() == 2 && eng.getTakeChannels() == 2, "take and render are stereo", (double) out.size());
        const auto pl = pitchIn (analyzeTake (out[0].data(), (int) out[0].size(), sr).analysis, 0.9, 1.2);
        const auto pr = pitchIn (analyzeTake (out[1].data(), (int) out[1].size(), sr).analysis, 0.9, 1.2);
        check (std::abs (pl.mean - 64.3f) < 0.12f && std::abs (pr.mean - 64.3f) < 0.12f, "both channels shifted +4", pr.mean);
        const double bal = rmsIn (out[1], sr, 0.9, 1.2) / rmsIn (out[0], sr, 0.9, 1.2);
        check (std::abs (bal - rmsIn (st[1], sr, 0.9, 1.2) / rmsIn (st[0], sr, 0.9, 1.2)) < 0.03, "stereo balance preserved (R/L)", bal);
        std::vector<float> L (1024, 0.0f), R (1024, 0.0f);
        float* outs[] = { L.data(), R.data() };
        eng.readPlayback ((juce::int64) sr, sr, outs, 2, 1024);
        check (L[10] == out[0][(size_t) sr + 10] && R[10] == out[1][(size_t) sr + 10], "playback is stereo", R[10]);
    }

    std::printf ("== transfer more parts later (merge keeps edits)\n");
    {
        Engine eng;
        const juce::int64 t0 = (juce::int64) (2.0 * sr);
        eng.loadTakeBlocking (x, sr, t0);
        auto en = eng.getNotes();
        en[(size_t) iC].pitchOffset = 2.0f;
        en[(size_t) iA].modScale = 0.0f;
        eng.setNotes (en);
        eng.renderBlocking();

        auto transfer = [&] (const std::vector<float>& audio, juce::int64 at)
        {
            eng.armTransfer (sr, 1);
            for (size_t i = 0; i < audio.size(); i += 512)
            {
                const float* chans[] = { audio.data() + i };
                eng.captureBlock (at + (juce::int64) i, chans, 1, (int) juce::jmin ((size_t) 512, audio.size() - i));
            }
            eng.transportStopped();
            return eng.waitUntilIdle (30000);
        };
        // a new phrase after the existing take (with a gap): G3 then B3
        const std::vector<SynthNote> later { { 0.05, 0.6, 55.0f, 0.0f, 0.0f, false }, { 0.70, 0.5, 59.0f, 0.0f, 0.0f, false } };
        const auto y = synthPhrase (sr, later, 1.3);
        const juce::int64 c0 = t0 + (juce::int64) (3.6 * sr);
        check (transfer (y, c0), "second transfer analysed", 1.0);
        auto merged = eng.getNotes();
        auto ma = eng.getAnalysis();
        check (eng.getTakeStart() == t0, "take start unchanged", (double) eng.getTakeStart());
        check (eng.getTakeLength() >= (int) (c0 - t0) + (int) y.size() - 1, "take extended to cover the new part (s)", eng.getTakeLength() / sr);
        int keptC = -1, keptA = -1, newG = -1, newB = -1;
        for (int i = 0; i < (int) merged.size(); ++i)
        {
            const double mid = 0.5 * (merged[(size_t) i].outStart + merged[(size_t) i].outEnd) / sr;
            if (std::abs (mid - 1.05) < 0.2) keptC = i;
            if (std::abs (mid - 0.40) < 0.2) keptA = i;
            if (std::abs (mid - 3.95) < 0.2) newG = i;
            if (std::abs (mid - 4.55) < 0.2) newB = i;
        }
        check (keptC >= 0 && std::abs (merged[(size_t) keptC].pitchOffset - 2.0f) < 1e-4f, "edit on old C4 kept", keptC >= 0 ? merged[(size_t) keptC].pitchOffset : -99);
        check (keptA >= 0 && merged[(size_t) keptA].modScale == 0.0f, "flattened vibrato on old A3 kept", keptA);
        check (newG >= 0 && std::abs (merged[(size_t) newG].origCenter - 55.0f) < 0.15f, "new G3 detected", newG >= 0 ? merged[(size_t) newG].origCenter : 0);
        check (newB >= 0 && std::abs (merged[(size_t) newB].origCenter - 59.0f) < 0.15f, "new B3 detected", newB >= 0 ? merged[(size_t) newB].origCenter : 0);

        // and a part BEFORE the take: the take grows backwards, frames stay aligned
        const juce::int64 c1 = t0 - (juce::int64) (1.5 * sr) + 77;
        check (transfer (y, c1), "third transfer (before the take) analysed", 1.0);
        const auto t2 = eng.getTakeStart();
        check (t2 <= c1 && (t0 - t2) % ma->hop == 0, "take start moved earlier, hop aligned", (double) (t0 - t2));
        merged = eng.getNotes();
        keptC = -1;
        for (int i = 0; i < (int) merged.size(); ++i)
        {
            const double mid = (0.5 * (merged[(size_t) i].outStart + merged[(size_t) i].outEnd) + (double) (t2 - t0)) / sr;
            if (std::abs (mid - 1.05) < 0.2) keptC = i;
        }
        check (keptC >= 0 && std::abs (merged[(size_t) keptC].pitchOffset - 2.0f) < 1e-4f, "edit still kept after growing backwards", keptC);
        eng.renderBlocking();
        const auto rendered = eng.getRenderedCopy()[0];
        const auto pc = pitchIn (analyzeTake (rendered.data(), (int) rendered.size(), sr).analysis,
                                 0.9 + (double) (t0 - t2) / sr, 1.2 + (double) (t0 - t2) / sr);
        check (std::abs (pc.mean - 62.3f) < 0.12f, "edited C4 still sounds +2 after merges", pc.mean);
    }

    std::printf ("== long transfer (no 10-minute cap: chunked capture)\n");
    {
        Engine eng;
        eng.armTransfer (sr, 2);
        std::vector<float> blockL (512), blockR (512);
        const int seconds = 40; // more than one 23.8 s capture chunk
        juce::int64 written = 0;
        for (int b = 0; b < (int) (seconds * sr / 512); ++b)
        {
            for (int i = 0; i < 512; ++i)
            {
                blockL[(size_t) i] = 0.1f * std::sin ((float) (written + i) * 0.05f);
                blockR[(size_t) i] = -blockL[(size_t) i];
            }
            const float* ch[] = { blockL.data(), blockR.data() };
            eng.captureBlock (1000 + written, ch, 2, 512);
            written += 512;
            if (b % 86 == 0) juce::Thread::sleep (35); // roughly real-time pacing for the chunk allocator
        }
        check (std::abs (eng.getCapturedSeconds() - seconds) < 0.05, "captured length (s)", eng.getCapturedSeconds());
        eng.disarmTransfer();
        check (eng.waitUntilIdle (60000), "long take analysed", 1.0);
        check (eng.getTakeChannels() == 2 && std::abs (eng.getTakeLength() / sr - seconds) < 0.05, "40 s stereo take installed (s)", eng.getTakeLength() / sr);
    }

    std::printf ("== drag & drop import + automatic line-up with the song\n");
    {
        std::vector<RealNote> phrase;
        for (int i = 0; i < 10; ++i)
            phrase.push_back ({ 0.3 + i * 1.6, 1.2, (float) (52 + (i * 5) % 12) + 0.2f, false, i % 3, (i + 1) % 3 });
        const auto vocal = synthRealVoice (sr, phrase, 16.5, 77);

        // the aligner on its own
        {
            const int s0 = (int) (7.3 * sr), m = (int) (4.0 * sr);
            const auto r = findChunkInTake (vocal.data(), (int) vocal.size(), vocal.data() + s0, m);
            check (r.offset == s0 && r.confidence > 0.99f, "finds a 4 s excerpt at the exact sample", (double) (r.offset - s0));
            std::vector<float> early ((size_t) m, 0.0f);
            std::copy (vocal.begin(), vocal.begin() + (m - (int) sr), early.begin() + (int) sr); // listening began 1 s before the clip
            const auto r2 = findChunkInTake (vocal.data(), (int) vocal.size(), early.data(), m);
            check (r2.offset == -(long long) sr && r2.confidence > 0.7f, "finds a clip that starts after listening began", (double) (r2.offset + (long long) sr));
            const auto other = synthRealVoice (sr, { { 0.2, 3.5, 47.0f, false, 2, 1 } }, 4.0, 5);
            const auto r3 = findChunkInTake (vocal.data(), (int) vocal.size(), other.data(), m);
            check (r3.confidence < 0.6f, "rejects audio that is not in the file (confidence)", r3.confidence);
        }

        // end to end through the engine, for a 44.1 kHz and a 48 kHz project
        const auto tmp = juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("noteforge_import_test.wav");
        writeWav (tmp, vocal, sr);
        for (double hostRate : { 44100.0, 48000.0 })
        {
            Engine eng;
            eng.importAudioFile (tmp);
            juce::Thread::sleep (50);
            check (eng.waitUntilIdle (60000) && eng.hasTake(), "  dropped file analysed", eng.hasTake());
            check (! eng.isSynced(), "  not lined up before the song has played", eng.isSynced());

            // the host plays the song; the clip sits at 12.5 s on the timeline. Playback starts at 11.0 s.
            const double clipAt = 12.5;
            std::vector<float> songAudio = vocal;
            if (hostRate != sr)
            {
                juce::LagrangeInterpolator li;
                std::vector<float> src = vocal; src.resize (vocal.size() + 64, 0.0f);
                const int skip = (int) std::lround (juce::LagrangeInterpolator::getBaseLatency() * hostRate / sr);
                songAudio.assign ((size_t) (vocal.size() * hostRate / sr) - 8 + (size_t) skip, 0.0f);
                li.process (sr / hostRate, src.data(), songAudio.data(), (int) songAudio.size());
                songAudio.erase (songAudio.begin(), songAudio.begin() + skip);
            }
            const auto clipHost = (juce::int64) std::llround (clipAt * hostRate);
            juce::int64 pos = (juce::int64) (11.0 * hostRate);
            std::vector<float> block (256);
            for (int b = 0; b < 4000 && ! eng.isSynced(); ++b)
            {
                for (int i = 0; i < 256; ++i)
                {
                    const auto k = pos + i - clipHost;
                    block[(size_t) i] = (k >= 0 && k < (juce::int64) songAudio.size()) ? songAudio[(size_t) k] : 0.0f;
                }
                const float* ch[] = { block.data() };
                eng.feedSync (pos, hostRate, ch, 1, 256);
                pos += 256;
                if (b % 64 == 0) juce::Thread::sleep (5);
            }
            for (int w = 0; w < 300 && ! eng.isSynced(); ++w) juce::Thread::sleep (20);
            const double expectStart = clipAt * sr;
            check (eng.isSynced() && std::abs ((double) eng.getTakeStart() - expectStart) <= 1.0,
                   ("  lined up automatically at 12.5 s (" + juce::String (hostRate / 1000.0, 1) + " kHz project), error in samples").toRawUTF8(),
                   (double) eng.getTakeStart() - expectStart);
            std::printf ("    status: %s\n", eng.getStatusText().toRawUTF8());

            // edited playback now lands exactly on the clip
            auto en = eng.getNotes();
            for (auto& n : en) n.pitchOffset = 2.0f;
            eng.setNotes (en);
            eng.renderBlocking();
            const auto rendered = eng.getRenderedCopy()[0];
            std::vector<float> out (1024, 0.123f);
            float* outs[] = { out.data() };
            const auto at = clipHost + (juce::int64) std::llround (5.0 * hostRate);
            eng.readPlayback (at, hostRate, outs, 1, 1024);
            const double p = (double) at * sr / hostRate - (double) eng.getTakeStart();
            const int k = (int) p;
            const double ref = rendered[(size_t) k] + (rendered[(size_t) k + 1] - rendered[(size_t) k]) * (p - k);
            check (std::abs (out[0] - ref) < 0.01, "  edited audio replaces the clip at the right spot", out[0] - ref);
        }
        tmp.deleteFile();
    }

    std::printf ("== engine state round-trip\n");
    {
        Engine eng;
        eng.loadTakeBlocking (x, sr, 44100 * 4);
        auto en = eng.getNotes();
        en[(size_t) iC].pitchOffset = 2.0f;
        en[(size_t) iA].modScale = 0.3f;
        eng.setNotes (en);
        eng.renderBlocking();
        const auto r1 = eng.getRenderedCopy()[0];
        const auto state = eng.saveState();
        juce::MemoryOutputStream mos;
        state.writeToStream (mos);
        std::printf ("  saved state: %d KB\n", (int) (mos.getDataSize() / 1024));

        Engine eng2;
        eng2.loadState (juce::ValueTree::readFromData (mos.getData(), mos.getDataSize()));
        eng2.renderBlocking();
        const auto r2 = eng2.getRenderedCopy()[0];
        const auto n2 = eng2.getNotes();
        check (n2.size() == en.size(), "notes restored", (double) n2.size());
        check (std::abs (n2[(size_t) iC].pitchOffset - 2.0f) < 1e-5f && std::abs (n2[(size_t) iA].modScale - 0.3f) < 1e-5f, "edits restored", n2[(size_t) iC].pitchOffset);
        check (eng2.getTakeStart() == 44100 * 4, "timeline position restored", (double) eng2.getTakeStart());
        double e = 0, s = 0;
        for (size_t i = 0; i < juce::jmin (r1.size(), r2.size()); ++i) { s += r1[i] * r1[i]; e += (r1[i] - r2[i]) * (r1[i] - r2[i]); }
        check (r1.size() == r2.size() && 10 * std::log10 (s / juce::jmax (e, 1e-20)) > 40.0, "re-render after reload matches (SNR dB)", 10 * std::log10 (s / juce::jmax (e, 1e-20)));

        std::vector<float> play (1024, 0.123f);
        float* outs[] = { play.data() };
        eng2.readPlayback (44100 * 4 + (juce::int64) (1.0 * sr), sr, outs, 1, 1024);
        check (std::abs (play[0] - r2[(size_t) sr]) < 1e-6f, "playback reads rendered take at timeline position", play[0]);
        std::fill (play.begin(), play.end(), 0.123f);
        eng2.readPlayback (0, sr, outs, 1, 1024);
        check (play[0] == 0.123f, "playback outside take leaves input untouched", play[0]);

        // project at 48 kHz playing a 44.1 kHz take: same timeline second, resampled
        std::fill (play.begin(), play.end(), 0.0f);
        const double hostRate = 48000.0;
        const auto hostPos = (juce::int64) std::llround ((4.0 + 1.0) * hostRate);
        eng2.readPlayback (hostPos, hostRate, outs, 1, 1024);
        double err = 0;
        for (int i = 0; i < 1024; ++i)
        {
            const double p = (double) (hostPos + i) * sr / hostRate - 44100.0 * 4;
            const int k = (int) p;
            const double ref = r2[(size_t) k] + (r2[(size_t) k + 1] - r2[(size_t) k]) * (p - k);
            err = juce::jmax (err, std::abs (play[(size_t) i] - ref));
        }
        check (err < 0.01, "plays correctly at a different project sample rate (max err)", err);
    }

    std::printf ("== piano key sound (sampled Steinway)\n");
    {
        PianoSynth piano;
        piano.prepare (sr);
        check (piano.hasSamples() && PianoSynth::bank().size() >= 29, "Steinway samples embedded (count)", (double) PianoSynth::bank().size());
        for (int key : { 40, 57, 61, 70, 76, 86 }) // includes keys between recorded notes
        {
            std::vector<float> buf ((size_t) (sr * 0.7), 0.0f);
            float* ch[] = { buf.data() };
            piano.noteOn (key);
            piano.render (ch, 1, (int) buf.size());
            piano.noteOff (key);
            piano.prepare (sr); // silence everything between keys
            const auto pa = pitchIn (analyzeTake (buf.data(), (int) buf.size(), sr).analysis, 0.08, 0.6);
            check (std::abs (pa.mean - (float) key) < 0.12f, ("key " + midiToName ((float) key) + " sounds at the right pitch").c_str(), pa.mean);
        }
        std::vector<float> buf ((size_t) (sr * 2.0), 0.0f);
        float* ch[] = { buf.data() };
        piano.noteOn (57); // A3
        piano.render (ch, 1, (int) (sr * 0.6));
        piano.noteOff (57);
        float* tail[] = { buf.data() + (size_t) (sr * 0.6) };
        piano.render (tail, 1, (int) (sr * 1.4));
        const double r0 = rmsIn (buf, sr, 0.05, 0.25), rEnd = rmsIn (buf, sr, 1.4, 1.6);
        check (r0 > 0.02 && r0 < 0.5, "piano level sensible (rms)", r0);
        check (rEnd < r0 * 0.01 && ! piano.isActive(), "damper stops the note after key release", rEnd);
        writeWav (outDir.getChildFile ("10_piano_A3.wav"), buf, sr);
        std::vector<float> scale ((size_t) (sr * 4.0), 0.0f);
        int t = 0;
        for (int key : { 60, 62, 64, 65, 67, 69, 71, 72 })
        {
            float* p[] = { scale.data() + (size_t) t };
            piano.noteOn (key);
            piano.render (p, 1, (int) (sr * 0.45));
            piano.noteOff (key);
            t += (int) (sr * 0.45);
        }
        float* rest[] = { scale.data() + (size_t) t };
        piano.render (rest, 1, (int) scale.size() - t);
        writeWav (outDir.getChildFile ("11_piano_scale.wav"), scale, sr);
    }
    std::printf ("== go back with the marker: preview from any point\n");
    {
        Engine eng;
        eng.loadTakeBlocking (x, sr, 0);
        eng.renderBlocking();
        const auto r = eng.getRenderedCopy()[0];
        eng.startPreview (1.5 * sr, (double) r.size());
        std::vector<float> pv (512, 0.0f);
        float* outs[] = { pv.data() };
        check (eng.readPreview (sr, outs, 1, 512), "preview playing", 1.0);
        check (std::abs (pv[100] - r[(size_t) (1.5 * sr) + 100]) < 1e-5f, "preview starts exactly at the marker", pv[100]);
        eng.startPreview (0.5 * sr, (double) r.size()); // drag the marker back while playing
        eng.readPreview (sr, outs, 1, 512);
        check (std::abs (pv[7] - r[(size_t) (0.5 * sr) + 7]) < 1e-5f, "moving the marker back jumps the preview back", pv[7]);
    }

#if NOTEFORGE_UI_SNAPSHOT
    std::printf ("== UI snapshot\n");
    {
        Engine eng;
        eng.loadTakeBlocking (x, sr, 0);
        auto en = eng.getNotes();
        en[(size_t) iC].pitchOffset = 2.0f;
        en[(size_t) iC].selected = true;
        eng.setNotes (en);
        eng.renderBlocking();
        EditorState es;
        NoteEditor ed (eng, es);
        ed.setSize (1200, 560);
        ed.setSpectrumVisible (true);
        ed.zoomToFit();
        auto img = ed.createComponentSnapshot (ed.getLocalBounds());
        juce::PNGImageFormat png;
        juce::FileOutputStream fos (outDir.getChildFile ("ui_snapshot.png"));
        fos.setPosition (0); fos.truncate();
        png.writeImageToStream (img, fos);
        std::printf ("  wrote ui_snapshot.png\n");
    }
    {
        // the whole plugin window, edit page then live page
        auto proc = std::make_unique<NoteForgeProcessor>();
        proc->setPlayConfigDetails (2, 2, sr, 512);
        proc->prepareToPlay (sr, 512);
        proc->engine.loadTakeBlocking (x, sr, 0);
        auto en = proc->engine.getNotes();
        en[(size_t) iE].selected = true;
        en[(size_t) iE].modScale = 0.0f;
        proc->engine.setNotes (en);
        proc->engine.renderBlocking();

        // run audio through the processor: edit mode replaces the take region with the render
        juce::AudioBuffer<float> buf (2, 512);
        juce::MidiBuffer midi;
        for (int ch = 0; ch < 2; ++ch) buf.clear (ch, 0, 512);
        proc->processBlock (buf, midi);
        check (proc->getLatencySamples() == 0, "edit mode reports zero latency", proc->getLatencySamples());
        proc->setNoteMask (0x091u);
        check (proc->getNoteMask() == 0x091u, "note picker mask stored in parameters", (double) proc->getNoteMask());
        proc->setPreset (9, 2);
        check (proc->getNoteMask() == presetMask (9, 2), "scale preset lights the right notes", (double) proc->getNoteMask());
        {
            // clicking a piano key in the UI queues a note that the audio thread plays
            proc->playNote (64, true);
            juce::AudioBuffer<float> silent (2, 512);
            juce::MidiBuffer m;
            double pianoEnergy = 0;
            for (int b = 0; b < 20; ++b)
            {
                silent.clear();
                proc->processBlock (silent, m);
                pianoEnergy += silent.getRMSLevel (0, 0, 512) + silent.getRMSLevel (1, 0, 512);
            }
            proc->playNote (64, false);
            check (pianoEnergy > 0.1, "clicked piano key is audible from the plugin output", pianoEnergy);
        }
    std::printf ("== ruler: wheel zoom and marker drag past the edge\n");
    {
        Engine eng;
        std::vector<RealNote> longer;
        for (int i = 0; i < 12; ++i) longer.push_back ({ 0.2 + i * 1.5, 1.2, 55.0f + (float) (i % 5), false, 0, 1 });
        eng.loadTakeBlocking (synthRealVoice (sr, longer, 18.5, 3), sr, 0);
        EditorState es;
        NoteEditor ed (eng, es);
        ed.setSize (1200, 560);
        ed.zoomToFit();
        auto src = juce::Desktop::getInstance().getMainMouseSource();
        auto mouse = [&] (juce::Point<float> p, juce::Point<float> downAt, bool dragged)
        {
            return juce::MouseEvent (src, p, juce::ModifierKeys (juce::ModifierKeys::leftButtonModifier), 1.0f, 0.0f, 0.0f, 0.0f, 0.0f,
                                     &ed, &ed, juce::Time::getCurrentTime(), downAt, juce::Time::getCurrentTime(), 1, dragged);
        };
        // wheel over the ruler (top 24 px) zooms around the mouse, without Ctrl
        const double spp0 = ed.getSamplesPerPixel();
        const juce::Point<float> rulerPt (700.0f, 10.0f);
        juce::MouseWheelDetails wd { 0.0f, 0.3f, false, false, false };
        juce::MouseEvent we (src, rulerPt, juce::ModifierKeys(), 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, &ed, &ed, juce::Time::getCurrentTime(), rulerPt,
                             juce::Time::getCurrentTime(), 0, false);
        const double anchorBefore = ed.getViewStart() + (700.0 - 56.0) * spp0;
        ed.mouseWheelMove (we, wd);
        const double spp1 = ed.getSamplesPerPixel();
        const double anchorAfter = ed.getViewStart() + (700.0 - 56.0) * spp1;
        check (spp1 < spp0 * 0.7, "wheel over the ruler zooms in (samples per pixel ratio)", spp1 / spp0);
        check (std::abs (anchorAfter - anchorBefore) < 2.0 * spp1, "  zoom keeps the point under the mouse in place (samples)", anchorAfter - anchorBefore);
        // scroll a bit right so there is room to go back, then drag the marker off the left edge
        juce::MouseWheelDetails right { 0.0f, -0.4f, false, false, false };
        juce::MouseEvent sh (src, { 600.0f, 300.0f }, juce::ModifierKeys (juce::ModifierKeys::shiftModifier), 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, &ed, &ed,
                             juce::Time::getCurrentTime(), { 600.0f, 300.0f }, juce::Time::getCurrentTime(), 0, false);
        for (int i = 0; i < 4; ++i) ed.mouseWheelMove (sh, right);
        const double startBefore = ed.getViewStart();
        const juce::Point<float> down (400.0f, 10.0f);
        ed.mouseDown (mouse (down, down, false));
        for (int i = 0; i < 30; ++i)
            ed.mouseDrag (mouse ({ 10.0f, 10.0f }, down, true)); // held past the left edge
        check (ed.getViewStart() < startBefore - 30 * 20 * spp1, "dragging the marker past the left edge scrolls the view left", (startBefore - ed.getViewStart()) / sr);
        check (ed.getMarker() >= ed.getViewStart() - 1.0 && ed.getMarker() <= ed.getViewStart() + 1200 * spp1, "  marker stays visible", (ed.getMarker() - ed.getViewStart()) / spp1);
        ed.mouseUp (mouse ({ 10.0f, 10.0f }, down, true));
    }

    std::printf ("== CPU meter with FL-style tiny slices, in real time\n");
    {
        auto proc3 = std::make_unique<NoteForgeProcessor>();
        proc3->setPlayConfigDetails (2, 2, sr, 256);
        proc3->prepareToPlay (sr, 256);
        if (auto* p = proc3->params.getParameter ("mode")) p->setValueNotifyingHost (1.0f);
        if (auto* p = proc3->params.getParameter ("liveEngine")) p->setValueNotifyingHost (1.0f); // high quality
        proc3->updateLatency();
        const auto v = synthRealVoice (sr, { { 0.05, 1.4, 57.3f, false, 0, 1 } }, 1.6, 9);
        const int slices[] = { 3, 13, 64, 7, 169 }; // sums to 256 = one device buffer
        juce::MidiBuffer m;
        double worstOldStyle = 0;
        double next = juce::Time::getMillisecondCounterHiRes();
        const double periodMs = 1000.0 * 256 / sr;
        for (size_t base = 0; base + 256 <= v.size(); base += 256)
        {
            size_t off = 0;
            for (int s : slices)
            {
                juce::AudioBuffer<float> b (2, s);
                for (int ch = 0; ch < 2; ++ch) b.copyFrom (ch, 0, v.data() + base + off, s);
                const auto t0 = juce::Time::getHighResolutionTicks();
                proc3->processBlock (b, m);
                const double ms = juce::Time::highResolutionTicksToSeconds (juce::Time::getHighResolutionTicks() - t0) * 1000.0;
                worstOldStyle = juce::jmax (worstOldStyle, ms / (1000.0 * s / sr)); // the old per-call "load"
                off += (size_t) s;
            }
            next += periodMs;
            while (juce::Time::getMillisecondCounterHiRes() < next) {} // wait for the next device buffer
        }
        std::printf ("  old per-call measure would show %.0f%% peak load; real-time measure: now %.1f%%, peak %.1f%%, overloads %d\n",
                     worstOldStyle * 100.0, proc3->cpuLoad.load() * 100.0, proc3->peakLoad.load() * 100.0, proc3->overBudgetCallbacks.load());
        check (proc3->overBudgetCallbacks.load() == 0, "no false overloads with sliced buffers", proc3->overBudgetCallbacks.load());
        check (proc3->peakLoad.load() < 0.3f && proc3->peakLoad.load() > 0.0f, "peak CPU reads a realistic value (fraction of real time)", proc3->peakLoad.load());
    }

    std::printf ("== FL link messages\n");
    {
        juce::MemoryOutputStream bin;
        for (auto [cmd, secs] : { std::pair<int, double> { 1, 83.4567 }, { 2, 3600.25 }, { 3, 0.0 }, { 1, 0.0 } })
        {
            const auto msg = FlBridge::makeMessage (cmd, secs);
            int c2 = 0; double s2 = 0;
            check (FlBridge::decodeMessage (msg, c2, s2) && c2 == cmd && std::abs (s2 - std::round (secs * 1000.0) / 1000.0) < 1e-9,
                   ("  round trip cmd " + std::to_string (cmd) + " at " + std::to_string (secs) + " s").c_str(), s2);
            bin.writeByte ((char) msg.getRawDataSize());
            bin.write (msg.getRawData(), (size_t) msg.getRawDataSize()); // exactly the bytes FL receives (F0 ... F7)
        }
        outDir.getChildFile ("fl_bridge_messages.bin").replaceWithData (bin.getData(), bin.getDataSize());
    }
    std::printf ("== host splits buffers but repeats the position (FL-style sub-blocks)\n");
    {
        struct FakePlayHead : juce::AudioPlayHead
        {
            juce::int64 pos = 0;
            bool playing = false;
            juce::Optional<PositionInfo> getPosition() const override
            {
                PositionInfo info;
                info.setIsPlaying (playing);
                info.setTimeInSamples (pos);
                info.setBpm (120.0);
                return info;
            }
        };
        const std::vector<RealNote> song { { 0.10, 0.9, 57.2f, false, 0, 1 }, { 1.05, 0.8, 60.3f, false, 2, 0 }, { 1.95, 0.9, 64.1f, false, 0, 0 } };
        const auto voice = synthRealVoice (sr, song, 3.0, 7);
        auto host = std::make_unique<NoteForgeProcessor>();
        host->setPlayConfigDetails (2, 2, sr, 512);
        host->prepareToPlay (sr, 512);
        FakePlayHead ph;
        host->setPlayHead (&ph);
        const juce::int64 songPos = (juce::int64) (10 * sr);
        juce::MidiBuffer m;
        auto runHost = [&] (std::vector<float>* collect)
        {
            for (size_t i = 0; i + 512 <= voice.size(); i += 512)
            {
                ph.playing = true;
                for (int part = 0; part < 2; ++part)
                {
                    ph.pos = songPos + (juce::int64) i; // both halves report the buffer start
                    juce::AudioBuffer<float> b (2, 256);
                    for (int ch = 0; ch < 2; ++ch) b.copyFrom (ch, 0, voice.data() + i + (size_t) part * 256, 256);
                    host->processBlock (b, m);
                    if (collect) collect->insert (collect->end(), b.getReadPointer (0), b.getReadPointer (0) + 256);
                }
            }
            ph.playing = false;
            juce::AudioBuffer<float> b (2, 256);
            b.clear();
            host->processBlock (b, m);
        };
        host->engine.armTransfer (sr, 2);
        runHost (nullptr);
        check (host->engine.waitUntilIdle (30000) && host->engine.hasTake(), "transfer finished", 1.0);
        const auto take = host->engine.getRenderedCopy();
        double e = 0, s = 0;
        const size_t n = juce::jmin (take.empty() ? 0 : take[0].size(), voice.size() - 1024);
        for (size_t i = 0; i < n; ++i) { s += voice[i] * voice[i]; e += (voice[i] - take[0][i]) * (voice[i] - take[0][i]); }
        check (host->engine.getTakeStart() == songPos && n > voice.size() - 2048, "take placed at the right song position", (double) host->engine.getTakeStart());
        check (10 * std::log10 (s / juce::jmax (e, 1e-20)) > 60.0, "transferred take is not chopped by sub-blocks (SNR dB)", 10 * std::log10 (s / juce::jmax (e, 1e-20)));

        std::vector<float> played;
        runHost (&played);
        e = 0; s = 0;
        for (size_t i = 0; i < juce::jmin (played.size(), voice.size()); ++i) { s += voice[i] * voice[i]; e += (voice[i] - played[i]) * (voice[i] - played[i]); }
        check (10 * std::log10 (s / juce::jmax (e, 1e-20)) > 60.0, "edit-mode playback does not stutter on sub-blocks (SNR dB)", 10 * std::log10 (s / juce::jmax (e, 1e-20)));
        // jump the playhead back to 1.0 s into the take while playing: NoteForge must follow the host
        {
            std::vector<float> jumped;
            const size_t from = (size_t) sr; // 1.0 s
            ph.playing = true;
            for (size_t i = 0; i < 40 * 512; i += 512)
            {
                ph.pos = songPos + (juce::int64) ((i < 20 * 512) ? 2 * sr + i : from + (i - 20 * 512));
                juce::AudioBuffer<float> b (2, 512);
                const size_t src = (size_t) (ph.pos - songPos);
                for (int ch = 0; ch < 2; ++ch) b.copyFrom (ch, 0, voice.data() + src, 512);
                host->processBlock (b, m);
                if (i >= 20 * 512) jumped.insert (jumped.end(), b.getReadPointer (0), b.getReadPointer (0) + 512);
            }
            e = 0; s = 0;
            for (size_t i = 0; i < jumped.size(); ++i) { const float v = voice[from + i]; s += v * v; e += (v - jumped[i]) * (v - jumped[i]); }
            check (10 * std::log10 (s / juce::jmax (e, 1e-20)) > 60.0, "moving the playhead back while playing is followed (SNR dB)", 10 * std::log10 (s / juce::jmax (e, 1e-20)));
        }
        host->setPlayHead (nullptr);
    }
    std::printf ("== the audio callback never allocates memory (crackle guard)\n");
    {
        struct PH : juce::AudioPlayHead
        {
            juce::int64 pos = 0; bool playing = false;
            juce::Optional<PositionInfo> getPosition() const override
            {
                PositionInfo i; i.setIsPlaying (playing); i.setTimeInSamples (pos); i.setBpm (120.0); return i;
            }
        };
        // breathy input: sung notes with loud breaths in between
        std::vector<RealNote> breathy { { 0.2, 0.8, 57.4f, false, 0, 1 }, { 1.6, 0.8, 61.3f, false, 2, 0 }, { 3.0, 0.8, 64.2f, false, 0, 0 } };
        auto v = synthRealVoice (sr, breathy, 4.0, 3);
        std::mt19937 rng (11);
        std::normal_distribution<float> g (0.0f, 1.0f);
        float lpf = 0.0f;
        for (double b0 : { 1.05, 2.45 })
            for (int i = (int) (b0 * sr); i < (int) ((b0 + 0.45) * sr); ++i)
            {
                lpf = 0.7f * lpf + 0.3f * g (rng);
                const float env = (float) std::sin (juce::MathConstants<double>::pi * (i - b0 * sr) / (0.45 * sr));
                v[(size_t) i] += 0.08f * lpf * env;
            }
        writeWav (outDir.getChildFile ("30_breathy_in.wav"), v, sr);

        auto proc2 = std::make_unique<NoteForgeProcessor>();
        proc2->setPlayConfigDetails (2, 2, sr, 256);
        proc2->prepareToPlay (sr, 256);
        PH ph;
        proc2->setPlayHead (&ph);
        juce::MidiBuffer m;
        auto run = [&] (const char* label, bool playing, std::vector<float>* collect)
        {
            int allocs = 0;
            for (size_t i = 0; i + 256 <= v.size(); i += 256)
            {
                ph.playing = playing;
                ph.pos = (juce::int64) i;
                juce::AudioBuffer<float> b (2, 256);
                for (int ch = 0; ch < 2; ++ch) b.copyFrom (ch, 0, v.data() + i, 256);
                const bool track = i > 20 * 256; // skip warm-up
                gAudioThreadAllocs = 0;
                gTrackAllocs = track;
                proc2->processBlock (b, m);
                gTrackAllocs = false;
                allocs += gAudioThreadAllocs.load();
                if (collect) collect->insert (collect->end(), b.getReadPointer (0), b.getReadPointer (0) + 256);
            }
            check (allocs == 0, (std::string ("  ") + label + ": allocations in the audio callback").c_str(), allocs);
        };
        auto setParam = [&] (const char* id, float v01) { if (auto* p = proc2->params.getParameter (id)) p->setValueNotifyingHost (v01); };
        setParam ("mode", 1.0f);
        if (auto* p = proc2->params.getParameter ("retune")) p->setValueNotifyingHost (p->convertTo0to1 (140.0f));
        setParam ("liveEngine", 1.0f); // high quality, as in the user's recording
        proc2->updateLatency();
        std::vector<float> hq;
        run ("live high quality, 140 ms", false, &hq);
        setParam ("liveEngine", 0.0f);
        proc2->updateLatency();
        run ("live low latency", false, nullptr);
        setParam ("mode", 0.0f);
        proc2->updateLatency();
        proc2->engine.loadTakeBlocking (v, sr, 0);
        auto en = proc2->engine.getNotes();
        for (auto& n : en) n.pitchOffset = 1.0f;
        proc2->engine.setNotes (en);
        proc2->engine.renderBlocking();
        run ("edit-mode playback", true, nullptr);
        proc2->setPlayHead (nullptr);

        // breaths through the high-quality engine at 140 ms / 100 %: no buzz, no crackle
        const int lat = proc2->liveTuner.latencySamples (false);
        std::vector<float> al (hq.begin() + juce::jmin ((size_t) lat, hq.size()), hq.end());
        al.resize (v.size(), 0.0f);
        writeWav (outDir.getChildFile ("31_breathy_live_hq_140ms.wav"), al, sr);
        const auto art = artifactStats (v, al, sr);
        std::printf ("  breath frames %d, turned buzzy %d, crackle frames %d, clicks in %d out %d\n", art.breathFrames, art.buzzInBreath,
                     art.staticFrames, countClicks (v, sr, 0.0, 4.0), countClicks (al, sr, 0.0, 4.0));
        check (art.buzzInBreath == 0, "  breaths stay breaths (no buzz)", art.buzzInBreath);
        check (countClicks (al, sr, 0.0, 4.0) <= countClicks (v, sr, 0.0, 4.0) + 1, "  no added clicks", countClicks (al, sr, 0.0, 4.0));
    }

        std::unique_ptr<juce::AudioProcessorEditor> ed (proc->createEditor());
        ed->setSize (1320, 800);
        auto write = [&] (const char* name)
        {
            auto img = ed->createComponentSnapshot (ed->getLocalBounds());
            juce::PNGImageFormat png;
            juce::FileOutputStream fos (outDir.getChildFile (name));
            fos.setPosition (0); fos.truncate();
            png.writeImageToStream (img, fos);
            std::printf ("  wrote %s\n", name);
        };
        write ("ui_full_edit.png");
        if (auto* prm = proc->params.getParameter ("mode")) prm->setValueNotifyingHost (1.0f);
        proc->updateLatency();
        check (proc->getLatencySamples() == proc->liveTuner.latencySamples (true) && proc->getLatencySamples() < 0.01 * sr, "live mode (recording engine) reports a small latency (samples)", proc->getLatencySamples());
        if (auto* eng = proc->params.getParameter ("liveEngine")) eng->setValueNotifyingHost (1.0f);
        proc->updateLatency();
        check (proc->getLatencySamples() == proc->liveTuner.latencySamples (false), "mixing engine reports its larger latency (samples)", proc->getLatencySamples());
        if (auto* eng = proc->params.getParameter ("liveEngine")) eng->setValueNotifyingHost (0.0f);
        proc->updateLatency();
        for (int i = 0; i < 40; ++i)
        {
            for (int ch = 0; ch < 2; ++ch)
                buf.copyFrom (ch, 0, x.data() + (size_t) (i * 512 + (int) (0.85 * sr)), 512);
            proc->processBlock (buf, midi);
        }
        juce::MessageManager::getInstance()->runDispatchLoopUntil (300);
        write ("ui_full_live.png");
        // bypassed: only the grey (sung) line keeps going
        if (auto* bp = proc->params.getParameter ("liveBypass")) bp->setValueNotifyingHost (1.0f);
        for (int i = 40; i < 120; ++i)
        {
            for (int ch = 0; ch < 2; ++ch)
                buf.copyFrom (ch, 0, x.data() + (size_t) (i * 512 + (int) (0.85 * sr)) % (x.size() - 600), 512);
            proc->processBlock (buf, midi);
        }
        juce::MessageManager::getInstance()->runDispatchLoopUntil (300);
        write ("ui_full_live_bypassed.png");
        ed.reset();
    }
#endif

#if NOTEFORGE_UI_SNAPSHOT
    const auto vst3Path = juce::SystemStats::getEnvironmentVariable ("NF_VST3_PATH", {});
    if (vst3Path.isNotEmpty())
    {
        std::printf ("== load built VST3 through a VST3 host\n");
        juce::VST3PluginFormat fmt;
        juce::OwnedArray<juce::PluginDescription> found;
        fmt.findAllTypesForFile (found, vst3Path);
        check (found.size() == 1, "VST3 module exposes one plugin", found.size());
        if (found.size() == 1)
        {
            std::printf ("  found: %s by %s (%s)\n", found[0]->name.toRawUTF8(), found[0]->manufacturerName.toRawUTF8(), found[0]->category.toRawUTF8());
            juce::String err;
            auto inst = fmt.createInstanceFromDescription (*found[0], sr, 512, err);
            check (inst != nullptr, ("plugin instantiated " + err).toRawUTF8(), inst != nullptr);
            if (inst != nullptr)
            {
                inst->setPlayConfigDetails (2, 2, sr, 512);
                inst->prepareToPlay (sr, 512);
                check (inst->getLatencySamples() == 0, "host sees zero latency in edit mode", inst->getLatencySamples());
                juce::AudioBuffer<float> b (2, 512);
                juce::MidiBuffer m;
                bool finite = true;
                double energy = 0;
                for (int blk = 0; blk < (int) (x.size() / 512); ++blk)
                {
                    for (int ch = 0; ch < 2; ++ch) b.copyFrom (ch, 0, x.data() + blk * 512, 512);
                    inst->processBlock (b, m);
                    for (int ch = 0; ch < 2; ++ch)
                        for (int i = 0; i < 512; ++i) { const float v = b.getSample (ch, i); finite &= std::isfinite (v); energy += v * v; }
                }
                check (finite && energy > 1.0, "processes audio (finite, non-silent output)", energy);
                juce::MemoryBlock st;
                inst->getStateInformation (st);
                inst->setStateInformation (st.getData(), (int) st.getSize());
                check (st.getSize() > 0, "state save/restore through the host (bytes)", (double) st.getSize());
                std::unique_ptr<juce::AudioProcessorEditor> ed (inst->createEditorIfNeeded());
                check (ed != nullptr, "editor opens inside the host", ed != nullptr);
                if (ed != nullptr)
                    std::printf ("  editor size %dx%d\n", ed->getWidth(), ed->getHeight());
                ed.reset();
                inst->releaseResources();
            }
        }
    }
#endif

    std::printf ("\n%s (%d failure%s)\n", failures == 0 ? "ALL PASSED" : "FAILURES", failures, failures == 1 ? "" : "s");
    return failures == 0 ? 0 : 1;
}

// ---------------------------------------------------------------------------------------------
// NF_REAL_WAV=<file>: analyse a real recording, render typical edits and report glitches.
static int realRecordingReport (const juce::File& wav, const juce::File& outDir)
{
    outDir.createDirectory();
    double sr = 44100.0;
    std::printf ("== real recording\n");
    auto chans = readAudioFile (wav, sr);
    if (chans.empty()) { std::printf ("  could not read file\n"); return 1; }
    std::vector<float> x (chans[0].size(), 0.0f);
    for (auto& c : chans) for (size_t i = 0; i < x.size(); ++i) x[i] += c[i] / (float) chans.size();

    float peak = 0; double dc = 0; int clipped = 0;
    for (float v : x) { peak = juce::jmax (peak, std::abs (v)); dc += v; if (std::abs (v) > 0.999f) ++clipped; }
    dc /= (double) x.size();
    std::printf ("  peak %.1f dBFS, DC %.5f, clipped samples %d\n", 20 * std::log10 (peak + 1e-9), dc, clipped);
    if (chans.size() == 2)
    {
        double d = 0, s = 0;
        for (size_t i = 0; i < x.size(); ++i) { d += std::pow (chans[0][i] - chans[1][i], 2); s += x[i] * x[i]; }
        std::printf ("  stereo difference: %.1f dB below the mono mix\n", 10 * std::log10 ((s + 1e-12) / (d + 1e-12)));
    }

    const auto t0 = juce::Time::getMillisecondCounterHiRes();
    const auto res = analyzeTake (x.data(), (int) x.size(), sr);
    std::printf ("  analysis took %.0f ms\n", juce::Time::getMillisecondCounterHiRes() - t0);
    const auto& a = res.analysis;
    int voiced = 0, jumps = 0, inNoteUnvoiced = 0, inNote = 0;
    std::vector<float> rmsDb;
    for (int k = 0; k < a.numFrames(); ++k)
    {
        rmsDb.push_back (20.0f * std::log10 (a.rms[(size_t) k] + 1e-9f));
        if (! a.voiced[(size_t) k]) continue;
        ++voiced;
        if (k > 0 && a.voiced[(size_t) k - 1] && std::abs (a.midi[(size_t) k] - a.midi[(size_t) k - 1]) > 3.0f) ++jumps;
    }
    for (auto& n : res.notes)
        for (int k = n.startFrame; k < n.endFrame; ++k) { ++inNote; if (! a.voiced[(size_t) k]) ++inNoteUnvoiced; }
    std::sort (rmsDb.begin(), rmsDb.end());
    std::printf ("  noise floor (10th pct frame level) %.1f dBFS, loud (90th pct) %.1f dBFS\n",
                 rmsDb[rmsDb.size() / 10], rmsDb[rmsDb.size() * 9 / 10]);
    std::printf ("  voiced frames %.1f%%, pitch jumps >3 st between frames: %d, unvoiced frames inside notes: %d of %d\n",
                 100.0 * voiced / a.numFrames(), jumps, inNoteUnvoiced, inNote);
    if (auto dump = juce::SystemStats::getEnvironmentVariable ("NF_REAL_DUMP", {}); dump.isNotEmpty())
    {
        const double d0 = dump.upToFirstOccurrenceOf (",", false, false).getDoubleValue();
        const double d1 = dump.fromFirstOccurrenceOf (",", false, false).getDoubleValue();
        for (int k = (int) (d0 * sr / a.hop); k < (int) (d1 * sr / a.hop) && k < a.numFrames(); ++k)
            std::printf ("    %.3fs v%d midi %6.2f rawHz %7.2f (%6.2f st) aper %.3f rms %.4f\n", k * a.hop / sr, a.voiced[(size_t) k],
                         a.midi[(size_t) k], a.rawHz[(size_t) k], a.rawHz[(size_t) k] > 0 ? hzToMidi (a.rawHz[(size_t) k]) : 0.0f,
                         a.aperiodicity[(size_t) k], a.rms[(size_t) k]);
    }
    {
        // live-tracker periodicity statistics on this voice: sung frames vs unpitched (breath/noise) frames
        PitchDetector det;
        det.prepare (sr, LiveTuner::autoRangeHz);
        int bins[2][6] = {}; int counts[2] = {};
        const float edges[] = { 0.05f, 0.10f, 0.15f, 0.20f, 0.30f };
        for (int k = 4; k < a.numFrames(); ++k)
        {
            const int s0 = k * a.hop - det.samplesNeeded();
            if (s0 < 0 || s0 + det.samplesNeeded() >= (int) x.size()) continue;
            const auto r = det.analyze (x.data() + s0);
            if (r.aperiodicity >= 0.2f || r.aperiodicity2 < 0.0f || r.rms < 0.002f) continue;
            const int cls = a.voiced[(size_t) k] ? 0 : 1;
            int b = 0; while (b < 5 && r.aperiodicity2 >= edges[b]) ++b;
            ++bins[cls][b]; ++counts[cls];
        }
        {
            // frame-to-frame pitch wander on sung (offline-voiced) frames, live detector
            float prevM = 0; int jc = 0; int jb[4] = {};
            for (int k = 4; k < a.numFrames(); ++k)
            {
                const int s0 = k * a.hop - det.samplesNeeded();
                if (s0 < 0 || s0 + det.samplesNeeded() >= (int) x.size()) { prevM = 0; continue; }
                const auto r = det.analyze (x.data() + s0);
                const float mm = (a.voiced[(size_t) k] && r.aperiodicity < 0.3f && r.freqHz > 0) ? hzToMidi (r.freqHz) : 0.0f;
                if (mm > 0 && prevM > 0) { const float d = std::abs (mm - prevM); ++jc; ++jb[d < 0.1f ? 0 : d < 0.2f ? 1 : d < 0.4f ? 2 : 3]; }
                prevM = mm;
            }
            if (jc) std::printf ("  sung pitch wander per frame: <0.1st %.1f%%  <0.2 %.1f%%  <0.4 %.1f%%  >=0.4 %.1f%%\n",
                                 100.0 * jb[0] / jc, 100.0 * jb[1] / jc, 100.0 * jb[2] / jc, 100.0 * jb[3] / jc);
        }
        for (int cls = 0; cls < 2; ++cls)
        {
            std::printf ("  2-period check, %s frames (%d):", cls == 0 ? "sung" : "breath/noise", counts[cls]);
            const char* lab[] = { "<.05", "<.10", "<.15", "<.20", "<.30", ">=.30" };
            for (int b = 0; b < 6; ++b) std::printf (" %s %.1f%%", lab[b], counts[cls] ? 100.0 * bins[cls][b] / counts[cls] : 0.0);
            std::printf ("\n");
        }
    }
    std::printf ("  %d notes detected:\n", (int) res.notes.size());
    int printed = 0;
    for (auto& n : res.notes)
    {
        const double s0 = n.outStart / sr, s1 = n.outEnd / sr;
        float lo = 200, hi = -200;
        for (int k = n.startFrame; k < n.endFrame; ++k)
            if (a.voiced[(size_t) k]) { lo = juce::jmin (lo, a.midi[(size_t) k]); hi = juce::jmax (hi, a.midi[(size_t) k]); }
        double lv = 0; int lc = 0;
        for (int k = n.startFrame; k < n.endFrame; ++k) { lv += a.rms[(size_t) k]; ++lc; }
        if (printed++ < 200)
            std::printf ("    %6.2f-%6.2fs  %-4s %+4.0fc  range %5.1f st  level %5.1f dBFS\n", s0, s1, midiToName (n.origCenter).c_str(),
                         (n.origCenter - std::round (n.origCenter)) * 100.0f, hi - lo, 20 * std::log10 (lv / juce::jmax (1, lc) + 1e-9));
    }

    std::vector<std::pair<double, double>> spans;
    for (auto& n : res.notes)
        if ((n.outEnd - n.outStart) / sr > 0.2) spans.push_back ({ n.outStart / sr + 0.05, n.outEnd / sr - 0.05 });
    const double total = x.size() / sr;
    const auto base = glitchStats (x, sr, spans);
    const int clicksIn = countClicks (x, sr, 0.0, total);
    std::printf ("  %-26s roughness %.3f  pitch-jumps %5.1f%%  level-jumps %5.1f%%  max-step %4.1f dB  clicks %d\n", "original",
                 base.roughness, base.pitchJumps, base.levelJumps, base.maxLevelJumpDb, clicksIn);

    // transitions between touching notes: abrupt pitch steps that the original does not have
    auto transitionSteps = [&] (const std::vector<float>& y)
    {
        const auto ya = analyzeTake (y.data(), (int) y.size(), sr).analysis;
        int bad = 0, total = 0;
        for (size_t i = 1; i < res.notes.size(); ++i)
        {
            if (res.notes[i - 1].endFrame != res.notes[i].startFrame) continue;
            ++total;
            const int b = res.notes[i].startFrame;
            float stepIn = 0, stepOut = 0;
            for (int k = b - 10; k <= b + 10; ++k)
            {
                if (k < 1 || k >= a.numFrames() || k >= ya.numFrames()) continue;
                if (a.voiced[(size_t) k] && a.voiced[(size_t) k - 1]) stepIn = juce::jmax (stepIn, std::abs (a.midi[(size_t) k] - a.midi[(size_t) k - 1]));
                if (ya.voiced[(size_t) k] && ya.voiced[(size_t) k - 1]) stepOut = juce::jmax (stepOut, std::abs (ya.midi[(size_t) k] - ya.midi[(size_t) k - 1]));
            }
            if (stepOut > stepIn + 0.35f) ++bad;
        }
        return std::make_pair (bad, total);
    };

    struct Case { const char* name; std::function<void (std::vector<Note>&)> edit; };
    const std::vector<Case> cases {
        { "no edit", [] (std::vector<Note>&) {} },
        { "correct pitch 100/50", [] (std::vector<Note>& v) { for (auto& n : v) n.selected = true; correctPitch (v, 1.0f, 0.5f, nullptr); } },
        { "all notes +2 st", [] (std::vector<Note>& v) { for (auto& n : v) n.pitchOffset += 2.0f; } },
        { "all notes -2 st", [] (std::vector<Note>& v) { for (auto& n : v) n.pitchOffset -= 2.0f; } },
        { "vibrato flattened", [] (std::vector<Note>& v) { for (auto& n : v) n.modScale = 0.0f; } },
        { "formant +2", [] (std::vector<Note>& v) { for (auto& n : v) n.formant = 2.0f; } },
    };
    std::vector<const float*> ins;
    for (auto& c : chans) ins.push_back (c.data());
    int idx = 0;
    for (auto& cs : cases)
    {
        auto e = res.notes;
        cs.edit (e);
        std::vector<std::vector<float>> outs;
        renderTake (ins, a, e, outs);
        std::vector<float> y (outs[0].size(), 0.0f);
        for (auto& c : outs) for (size_t i = 0; i < y.size(); ++i) y[i] += c[i] / (float) outs.size();
        const auto g = glitchStats (y, sr, spans);
        const auto tr = transitionSteps (y);
        std::printf ("  %-26s roughness %.3f  pitch-jumps %5.1f%%  level-jumps %5.1f%%  max-step %4.1f dB  clicks %d  new steps at transitions %d/%d\n", cs.name,
                     g.roughness, g.pitchJumps, g.levelJumps, g.maxLevelJumpDb, countClicks (y, sr, 0.0, total), tr.first, tr.second);
        writeWav (outDir.getChildFile ("edit" + juce::String (idx++) + "_" + juce::String (cs.name).replaceCharacters (" /%+", "___p") + ".wav"), y, sr);
    }

    for (bool lowLat : { true, false })
    {
        LiveTuner tuner;
        tuner.prepare (sr, LiveTuner::autoRangeHz);
        LiveTuner::Params p;
        p.scale = Scale::fromPreset (0, 0);
        p.retuneMs = (float) juce::SystemStats::getEnvironmentVariable ("NF_REAL_RETUNE", "20").getDoubleValue();
        p.lowLatency = lowLat;
        std::vector<float> y (x.size());
        for (size_t i = 0; i < x.size(); i += 512)
            tuner.process (x.data() + i, y.data() + i, (int) juce::jmin ((size_t) 512, x.size() - i), p);
        const int lat = tuner.latencySamples (lowLat);
        std::vector<float> al (y.begin() + lat, y.end());
        al.resize (x.size(), 0.0f);
        const auto g = glitchStats (al, sr, spans);
        std::printf ("  live %-21s roughness %.3f  pitch-jumps %5.1f%%  level-jumps %5.1f%%  max-step %4.1f dB  clicks %d\n",
                     lowLat ? "low latency" : "high quality", g.roughness, g.pitchJumps, g.levelJumps, g.maxLevelJumpDb,
                     countClicks (al, sr, 0.0, total));
        const auto art = artifactStats (x, al, sr);
        if (auto sd = juce::SystemStats::getEnvironmentVariable ("NF_STATIC_DUMP", {}); sd.isNotEmpty())
        {
            const double d0 = sd.upToFirstOccurrenceOf (",", false, false).getDoubleValue(), d1 = sd.fromFirstOccurrenceOf (",", false, false).getDoubleValue();
            const auto ai = analyzeTake (x.data(), (int) x.size(), sr).analysis;
            const int hop = ai.hop;
            for (int k = (int) (d0 * sr / hop); k < (int) (d1 * sr / hop); ++k)
            {
                double hi = 0, ho = 0, ri = 0, ro = 0, maxJump = 0;
                for (int i = k * hop - hop; i < k * hop + hop; ++i)
                {
                    if (i < 1 || i >= (int) x.size()) continue;
                    hi += std::pow (x[(size_t) i] - x[(size_t) i - 1], 2); ho += std::pow (al[(size_t) i] - al[(size_t) i - 1], 2);
                    ri += x[(size_t) i] * x[(size_t) i]; ro += al[(size_t) i] * al[(size_t) i];
                    maxJump = juce::jmax (maxJump, (double) std::abs (al[(size_t) i] - al[(size_t) i - 1]));
                }
                std::printf ("      %.3fs in: v%d %5.2f st aper %.2f lvl %5.1f dB hf %5.1f | out: lvl %5.1f dB hf %5.1f maxjump %.4f\n", k * hop / sr,
                             ai.voiced[(size_t) k], ai.midi[(size_t) k], ai.aperiodicity[(size_t) k], 10 * std::log10 (ri / (2 * hop) + 1e-12),
                             10 * std::log10 (hi / (2 * hop) + 1e-12), 10 * std::log10 (ro / (2 * hop) + 1e-12), 10 * std::log10 (ho / (2 * hop) + 1e-12), maxJump);
            }
        }
        std::printf ("    retune %.0f ms: breath frames %d, breath turned buzzy %d, static/crackle frames %d of %d, late grains %d\n", p.retuneMs,
                     art.breathFrames, art.buzzInBreath, art.staticFrames, art.frames, tuner.lateGrains.load());
        if (! art.worst.empty())
        {
            std::printf ("    static at:");
            for (double t : art.worst) std::printf (" %.2fs", t);
            std::printf ("\n");
        }
        writeWav (outDir.getChildFile (lowLat ? "live_lowlatency.wav" : "live_highquality.wav"), al, sr);
    }

    // editor screenshots: the whole take and a few close-ups
    auto snapshot = [&] (int s0, int s1, const juce::String& name)
    {
        Engine eng;
        std::vector<float> part (x.begin() + s0, x.begin() + s1);
        eng.loadTakeBlocking (part, sr, 0);
        eng.renderBlocking();
        EditorState es;
        NoteEditor ed (eng, es);
        ed.setSize (1600, 700);
        ed.zoomToFit();
        auto img = ed.createComponentSnapshot (ed.getLocalBounds());
        juce::PNGImageFormat png;
        auto f = outDir.getChildFile (name);
        f.deleteFile();
        juce::FileOutputStream fos (f);
        png.writeImageToStream (img, fos);
    };
    snapshot (0, (int) x.size(), "editor_full.png");
    const int win = (int) (8.0 * sr);
    for (int i = 0; i < 3; ++i)
    {
        const int s0 = (int) ((double) i / 3.0 * (double) juce::jmax (0, (int) x.size() - win));
        snapshot (s0, juce::jmin ((int) x.size(), s0 + win), "editor_part" + juce::String (i + 1) + ".png");
        std::printf ("  editor_part%d.png = %.1f-%.1f s\n", i + 1, s0 / sr, (s0 + win) / sr);
    }
    return 0;
}
