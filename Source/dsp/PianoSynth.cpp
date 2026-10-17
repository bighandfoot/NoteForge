#include "PianoSynth.h"
#include <juce_audio_formats/juce_audio_formats.h>
#include "PianoData.h"
#include <algorithm>
#include <cmath>

namespace nf
{
const std::vector<PianoSynth::Sample>& PianoSynth::bank()
{
    static const std::vector<Sample> samples = []
    {
        std::vector<Sample> out;
        int size = 0;
        const char* manifest = PianoData::getNamedResource ("manifest_txt", size);
        if (manifest == nullptr)
            return out;
        juce::FlacAudioFormat flac;
        for (auto line : juce::StringArray::fromLines (juce::String::fromUTF8 (manifest, size)))
        {
            const auto parts = juce::StringArray::fromTokens (line.trim(), " ", "");
            if (parts.size() != 3)
                continue;
            int dataSize = 0;
            const auto resName = parts[2].replaceCharacter ('.', '_');
            const char* data = PianoData::getNamedResource (resName.toRawUTF8(), dataSize);
            if (data == nullptr)
                continue;
            std::unique_ptr<juce::AudioFormatReader> r (
                flac.createReaderFor (new juce::MemoryInputStream (data, (size_t) dataSize, false), true));
            if (r == nullptr)
                continue;
            Sample s;
            s.root = parts[0].getIntValue();
            s.cents = parts[1].getFloatValue();
            s.rate = r->sampleRate;
            juce::AudioBuffer<float> buf (1, (int) r->lengthInSamples);
            r->read (&buf, 0, buf.getNumSamples(), 0, true, false);
            s.data.assign (buf.getReadPointer (0), buf.getReadPointer (0) + buf.getNumSamples());
            out.push_back (std::move (s));
        }
        std::sort (out.begin(), out.end(), [] (const Sample& a, const Sample& b) { return a.root < b.root; });
        return out;
    }();
    return samples;
}

PianoSynth::PianoSynth()
{
    bank(); // decode up front (message thread), never on the audio thread
}

void PianoSynth::prepare (double sampleRate)
{
    sr = sampleRate;
    for (auto& v : voices) v.midi = -1;
}

void PianoSynth::noteOn (int midi, float velocity)
{
    const auto& b = bank();
    if (b.empty())
        return;
    const Sample* best = &b.front();
    for (auto& s : b)
        if (std::abs (s.root - midi) < std::abs (best->root - midi)) best = &s;

    Voice* v = nullptr;
    for (auto& c : voices) if (c.midi == midi) v = &c;                 // re-strike the same key
    if (v == nullptr) for (auto& c : voices) if (c.midi < 0) { v = &c; break; }
    if (v == nullptr)
        v = &*std::min_element (voices.begin(), voices.end(), [] (const Voice& a, const Voice& c) { return a.age < c.age; });

    v->midi = midi;
    v->sample = best;
    v->pos = 0.0;
    v->step = std::pow (2.0, (midi - best->root) / 12.0 - best->cents / 1200.0) * best->rate / sr;
    v->gain = velocity * 0.4f; // audition level: 60% quieter than the raw samples
    v->release = 1.0f;
    // the top ~20 keys of a grand have no dampers and keep ringing after release
    const double tau = midi >= 89 ? 1.2 : 0.11;
    v->releaseCoef = (float) std::exp (-1.0 / (tau * sr));
    v->held = true;
    v->age = ++clock;
}

void PianoSynth::noteOff (int midi)
{
    for (auto& v : voices)
        if (v.midi == midi) v.held = false;
}

void PianoSynth::allNotesOff()
{
    for (auto& v : voices) v.held = false;
}

bool PianoSynth::isActive() const noexcept
{
    for (auto& v : voices) if (v.midi >= 0) return true;
    return false;
}

void PianoSynth::render (float* const* out, int numCh, int n)
{
    for (auto& v : voices)
    {
        if (v.midi < 0 || v.sample == nullptr) continue;
        const auto& d = v.sample->data;
        const double last = (double) d.size() - 2.0;
        for (int i = 0; i < n; ++i)
        {
            if (v.pos >= last) { v.midi = -1; break; }
            const int k = (int) v.pos;
            const float t = (float) (v.pos - k);
            float y = d[(size_t) k] + (d[(size_t) k + 1] - d[(size_t) k]) * t;
            if (! v.held) v.release *= v.releaseCoef;
            y *= v.gain * v.release;
            for (int ch = 0; ch < numCh; ++ch) out[ch][i] += y;
            v.pos += v.step;
        }
        if (v.release < 1.0e-3f) // -60 dB: free the voice
            v.midi = -1;
    }
}
} // namespace nf
