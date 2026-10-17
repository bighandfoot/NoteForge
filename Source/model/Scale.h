#pragma once
#include <array>
#include <string>
#include <vector>

namespace nf
{
struct ScaleDef
{
    const char* name;
    std::array<int, 12> degrees; // 1 = pitch class is in the scale (relative to the root)
};

const std::vector<ScaleDef>& allScales();
std::vector<std::string> keyNames();

// Bit set of allowed pitch classes for a preset (bit 0 = C ... bit 11 = B).
unsigned presetMask (int root, int type);
constexpr unsigned allNotesMask = 0xFFFu;

// The notes pitch correction may snap to. `root`/`type` name the preset it came from;
// `mask` is authoritative (the user can toggle individual notes on the note picker).
struct Scale
{
    int root = 0;
    int type = 0;
    unsigned mask = allNotesMask;

    static Scale fromPreset (int root, int type) { return { root, type, presetMask (root, type) }; }
    bool contains (int midiNote) const;
    // Nearest MIDI pitch that belongs to the scale (float in, integer-valued float out).
    float snap (float midi) const;
    // Next scale note strictly above/below `midiNote` (an integer note).
    int step (int midiNote, int direction) const;
};

std::string midiToName (float midi);  // e.g. "A4"
float hzToMidi (float hz);
float midiToHz (float midi);

// Krumhansl–Schmuckler key estimate from (pitch, weight) pairs. Returns a Scale with type 1 (major) or 2 (minor).
Scale estimateKey (const std::vector<std::pair<float, float>>& pitchWeights);
} // namespace nf
