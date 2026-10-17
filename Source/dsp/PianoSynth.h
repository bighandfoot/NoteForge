#pragma once
#include <array>
#include <cstdint>
#include <vector>

namespace nf
{
// Sampled piano used to audition notes when piano keys are clicked.
// Plays real Steinway Model B recordings (University of Iowa Electronic Music Studios, mf),
// one every three semitones, each retuned to exact equal temperament.
class PianoSynth
{
public:
    PianoSynth();
    void prepare (double sampleRate);
    void noteOn (int midi, float velocity = 0.8f);
    void noteOff (int midi);
    void allNotesOff();
    // Adds the piano to every channel.
    void render (float* const* out, int numChannels, int numSamples);
    bool isActive() const noexcept;
    bool hasSamples() const noexcept { return ! bank().empty(); }

    struct Sample
    {
        int root = 60;
        float cents = 0.0f;  // measured tuning of the recording, corrected on playback
        double rate = 44100.0;
        std::vector<float> data;
    };
    static const std::vector<Sample>& bank(); // decoded once per process from the embedded FLAC files

private:
    static constexpr int numVoices = 12;
    struct Voice
    {
        int midi = -1;
        const Sample* sample = nullptr;
        double pos = 0.0, step = 1.0;
        float gain = 1.0f, release = 1.0f, releaseCoef = 1.0f;
        bool held = false;
        uint32_t age = 0;
    };
    std::array<Voice, numVoices> voices;
    double sr = 44100.0;
    uint32_t clock = 0;
};
} // namespace nf
