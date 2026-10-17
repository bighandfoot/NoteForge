// Offline tool: turns the University of Iowa Steinway Model B "mf" recordings into the compact
// mono FLAC set that is embedded in the plugin for piano-key auditioning.
// Usage: NoteForgeSamplePrep <raw aiff dir> <output dir>
#include <juce_audio_formats/juce_audio_formats.h>
#include "dsp/Analysis.h"
#include "dsp/FFT.h"
#include <complex>
#include <cstdio>
#include <map>

static int noteNameToMidi (const juce::String& n)
{
    static const std::map<juce::String, int> pcs { { "C", 0 }, { "Db", 1 }, { "D", 2 }, { "Eb", 3 }, { "E", 4 }, { "F", 5 },
                                                   { "Gb", 6 }, { "G", 7 }, { "Ab", 8 }, { "A", 9 }, { "Bb", 10 }, { "B", 11 } };
    const auto letters = n.retainCharacters ("ABCDEFGb");
    const int octave = n.getTrailingIntValue();
    const auto it = pcs.find (letters);
    return it == pcs.end() ? -1 : (octave + 1) * 12 + it->second;
}

int main (int argc, char** argv)
{
    if (argc < 3) { std::printf ("usage: SamplePrep <raw dir> <out dir>\n"); return 1; }
    const juce::File rawDir (juce::String::fromUTF8 (argv[1])), outDir (juce::String::fromUTF8 (argv[2]));
    outDir.createDirectory();

    juce::AiffAudioFormat aiff;
    struct Item { int midi; std::vector<float> mono; double sr; };
    std::vector<Item> items;
    float globalPeak = 0.0f;

    for (const auto& f : rawDir.findChildFiles (juce::File::findFiles, false, "Piano.mf.*.aiff"))
    {
        const auto name = f.getFileNameWithoutExtension().fromLastOccurrenceOf (".", false, false); // e.g. "Eb4"
        const int midi = noteNameToMidi (name);
        std::unique_ptr<juce::AudioFormatReader> r (aiff.createReaderFor (f.createInputStream().release(), true));
        if (r == nullptr || midi < 0) { std::printf ("skip %s\n", f.getFileName().toRawUTF8()); continue; }
        juce::AudioBuffer<float> buf ((int) r->numChannels, (int) r->lengthInSamples);
        r->read (&buf, 0, buf.getNumSamples(), 0, true, true);
        Item it { midi, std::vector<float> ((size_t) buf.getNumSamples()), r->sampleRate };
        for (int i = 0; i < buf.getNumSamples(); ++i)
        {
            float v = 0.0f;
            for (int ch = 0; ch < buf.getNumChannels(); ++ch) v += buf.getSample (ch, i);
            it.mono[(size_t) i] = v / (float) buf.getNumChannels();
        }
        float peak = 0.0f;
        for (float v : it.mono) peak = juce::jmax (peak, std::abs (v));
        // trim: start 3 ms before the hammer hits, end when the note has decayed to -60 dB (max 4.5 s)
        size_t onset = 0;
        while (onset < it.mono.size() && std::abs (it.mono[onset]) < 0.03f * peak) ++onset;
        const size_t start = onset > (size_t) (0.003 * it.sr) ? onset - (size_t) (0.003 * it.sr) : 0;
        size_t end = it.mono.size();
        const size_t win = (size_t) (0.05 * it.sr);
        for (size_t s = onset; s + win < it.mono.size(); s += win)
        {
            double e = 0;
            for (size_t i = s; i < s + win; ++i) e += it.mono[i] * it.mono[i];
            if (std::sqrt (e / (double) win) < 0.001 * peak) { end = s + win; break; }
        }
        end = juce::jmin (end, start + (size_t) (4.5 * it.sr));
        std::vector<float> trimmed (it.mono.begin() + (long) start, it.mono.begin() + (long) end);
        const size_t fade = juce::jmin (trimmed.size() / 4, (size_t) (0.25 * it.sr));
        for (size_t i = 0; i < fade; ++i)
            trimmed[trimmed.size() - 1 - i] *= (float) i / (float) fade;
        it.mono = std::move (trimmed);
        globalPeak = juce::jmax (globalPeak, peak);
        items.push_back (std::move (it));
    }
    std::sort (items.begin(), items.end(), [] (const Item& a, const Item& b) { return a.midi < b.midi; });

    juce::String manifest;
    juce::FlacAudioFormat flac;
    const float gain = 0.89f / juce::jmax (1.0e-6f, globalPeak); // one gain for all: keeps the natural balance
    for (auto& it : items)
    {
        // measure the real tuning of each recording so playback can correct it
        float cents = 0.0f;
        if (nf::midiToHz ((float) it.midi) > 60.0f)
        {
            const int len = (int) juce::jmin (it.mono.size(), (size_t) (0.6 * it.sr));
            const auto res = nf::analyzeTake (it.mono.data(), len, it.sr);
            double s = 0; int c = 0;
            for (int k = 0; k < res.analysis.numFrames(); ++k)
                if (res.analysis.voiced[(size_t) k] && k * res.analysis.hop > 0.05 * it.sr)
                {
                    const float d = res.analysis.midi[(size_t) k] - (float) it.midi;
                    if (std::abs (d) < 0.5f) { s += d; ++c; }
                }
            if (c > 4) cents = (float) (100.0 * s / c);
        }
        if (nf::midiToHz ((float) it.midi) > 1300.0f)
        {
            // above the pitch tracker's range: interpolated peak of a long FFT near the expected fundamental
            nf::FFT fft (16);
            const int N = fft.size();
            std::vector<std::complex<float>> z ((size_t) N);
            const int s0 = (int) (0.05 * it.sr);
            const int len = juce::jmin (N, (int) it.mono.size() - s0);
            for (int i = 0; i < len; ++i)
                z[(size_t) i] = { it.mono[(size_t) (s0 + i)] * (0.5f - 0.5f * std::cos (6.2831853f * (float) i / (float) len)), 0.0f };
            fft.forward (z.data());
            const double binHz = it.sr / N, f0 = nf::midiToHz ((float) it.midi);
            const int lo = (int) (f0 * 0.985 / binHz), hi = (int) (f0 * 1.07 / binHz); // stretch tuning goes sharp up here
            int best = lo;
            for (int k = lo; k <= hi; ++k) if (std::abs (z[(size_t) k]) > std::abs (z[(size_t) best])) best = k;
            const double a = std::abs (z[(size_t) best - 1]), b = std::abs (z[(size_t) best]), c = std::abs (z[(size_t) best + 1]);
            const double den = a - 2.0 * b + c;
            const double peak = (best + (std::abs (den) > 1e-12 ? 0.5 * (a - c) / den : 0.0)) * binHz;
            cents = (float) (1200.0 * std::log2 (peak / f0));
        }

        const auto file = outDir.getChildFile ("mf_" + juce::String (it.midi) + ".flac");
        file.deleteFile();
        std::unique_ptr<juce::OutputStream> os = std::make_unique<juce::FileOutputStream> (file);
        auto w = flac.createWriterFor (os, juce::AudioFormatWriterOptions{}.withSampleRate (it.sr).withNumChannels (1).withBitsPerSample (16));
        juce::AudioBuffer<float> b (1, (int) it.mono.size());
        for (int i = 0; i < b.getNumSamples(); ++i) b.setSample (0, i, it.mono[(size_t) i] * gain);
        w->writeFromAudioSampleBuffer (b, 0, b.getNumSamples());
        w.reset();
        manifest << it.midi << " " << juce::String (cents, 1) << " " << file.getFileName() << "\n";
        std::printf ("%3d %-6s %5.2fs  tuning %+5.1f c  %6.0f KB\n", it.midi, nf::midiToName ((float) it.midi).c_str(),
                     it.mono.size() / it.sr, cents, file.getSize() / 1024.0);
    }
    outDir.getChildFile ("manifest.txt").replaceWithText (manifest);
    return 0;
}
