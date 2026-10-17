#pragma once
#include "Scale.h"
#include <cstdint>
#include <set>
#include <vector>

namespace nf
{
// Result of pitch analysis on a captured take. Frame k describes the audio centred at sample k * hop.
struct Analysis
{
    double sampleRate = 44100.0;
    int hop = 256;
    int numSamples = 0;

    std::vector<float> midi;      // f0 as fractional MIDI pitch; meaningful only where voiced
    std::vector<float> rms;       // linear RMS per frame
    std::vector<uint8_t> voiced;
    float maxRms = 0.0f;

    // Raw detector output before cleanup, kept so a later transfer can re-analyse only what changed.
    std::vector<float> rawHz, aperiodicity;

    // Pitch marks for PSOLA: glottal-pulse aligned in voiced regions, fixed spacing elsewhere.
    std::vector<int> marks;
    std::vector<uint8_t> markVoiced;

    int numFrames() const noexcept { return (int) midi.size(); }
    double frameToSample (double frame) const noexcept { return frame * hop; }
    double sampleToFrame (double sample) const noexcept { return sample / hop; }
    float midiAtFrame (double frame) const;     // interpolated, voiced frames only
    bool voicedAtFrame (int frame) const noexcept
    {
        return frame >= 0 && frame < numFrames() && voiced[(size_t) frame] != 0;
    }
};

// A detected note ("blob") plus every edit the user made to it.
struct Note
{
    int startFrame = 0, endFrame = 0; // analysis frames [start, end) in the original take
    float origCenter = 60.0f;         // perceived pitch centre of the original performance

    float pitchOffset = 0.0f;         // semitones added to the centre
    float modScale = 1.0f;            // vibrato / fast modulation amount (1 = original)
    float driftScale = 1.0f;          // slow drift amount (1 = original, 0 = flat)
    float formant = 0.0f;             // formant shift in semitones
    float gainDb = 0.0f;
    bool muted = false;

    double outStart = 0.0, outEnd = 0.0; // position on the output timeline, in samples
    bool selected = false;

    float center() const noexcept { return origCenter + pitchOffset; }
    double inStart (int hop) const noexcept { return (double) startFrame * hop; }
    double inEnd (int hop) const noexcept { return (double) endFrame * hop; }
};

// Piecewise-linear mapping between original and edited time, built from note boundaries.
class TimeMap
{
public:
    void build (const std::vector<Note>& notes, int hop, int numSamples);
    double toOutput (double inSample) const;
    double toInput (double outSample) const;

private:
    std::vector<double> in, out;
};

// Per-frame control curves consumed by the renderer (indexed by ORIGINAL frame).
struct RenderCurves
{
    std::vector<float> shift;   // semitones
    std::vector<float> formant; // semitones
    std::vector<float> gain;    // linear
};

float computeCenter (const Analysis& a, int startFrame, int endFrame);
// Slow component of a note's pitch (vibrato removed). Indexed relative to note.startFrame.
std::vector<float> slowPitch (const Analysis& a, const Note& n);
// Pitch the edited note will have at `frame` (needs slowPitch(a, n)). Returns 0 for unvoiced frames.
float editedPitch (const Analysis& a, const Note& n, const std::vector<float>& slow, int frame);
RenderCurves buildCurves (const Analysis& a, const std::vector<Note>& notes);

// ---- edit operations (all operate on notes sorted by startFrame) ----
double minNoteSamples (const Analysis& a);
// Moves the selected notes by `delta` samples from the `initial` state. Touching neighbours stretch to follow.
void moveNotesInTime (std::vector<Note>& notes, const std::vector<Note>& initial, double delta, const Analysis& a);
void stretchNoteEdge (std::vector<Note>& notes, const std::vector<Note>& initial, int index, bool leftEdge,
                      double delta, const Analysis& a);
bool splitNote (std::vector<Note>& notes, int index, int frame, const Analysis& a);
bool mergeWithNext (std::vector<Note>& notes, int index, const Analysis& a);
void correctPitch (std::vector<Note>& notes, float centerAmount, float driftAmount, const Scale* scale);
void quantizeTime (std::vector<Note>& notes, double gridSamples, double gridOffset, float amount, const Analysis& a);
void resetNote (Note& n);                              // pitch/formant/gain edits only
void resetTiming (std::vector<Note>& notes, int hop);  // all notes back to original timing
} // namespace nf
