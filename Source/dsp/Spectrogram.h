#pragma once
#include <vector>

namespace nf
{
// Log-frequency magnitude spectrogram whose rows line up with the piano roll's semitone grid.
struct SpectrogramData
{
    int width = 0, height = 0;   // columns = time frames, rows = pitch bins (row 0 = highest pitch)
    double hopSamples = 512.0;
    float minMidi = 24.0f, maxMidi = 108.0f;
    int rowsPerSemitone = 4;
    float peak = 0.0f;           // magnitude mapped to full brightness
    std::vector<float> level;    // 0..1, row-major
};

SpectrogramData computeSpectrogram (const float* x, int numSamples, double sampleRate, int hopSamples,
                                    float minMidi = 24.0f, float maxMidi = 108.0f, int rowsPerSemitone = 4);

// Recomputes columns [c0, c1) of an existing spectrogram of the same signal length, keeping its brightness scale.
void updateSpectrogram (SpectrogramData& s, const float* x, int numSamples, double sampleRate, int c0, int c1);
} // namespace nf
