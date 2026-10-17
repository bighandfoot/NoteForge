#pragma once
#include <juce_gui_basics/juce_gui_basics.h>
#include <functional>

namespace nf
{
// One-octave piano: click keys to choose which notes pitch correction may snap to.
// Lit keys are allowed; the root of the selected key is marked with a dot.
class NotePicker : public juce::Component, public juce::SettableTooltipClient
{
public:
    std::function<unsigned()> getMask;
    std::function<void (unsigned)> setMask;
    std::function<int()> getRoot;
    std::function<void (int midi, bool on)> playNote; // piano sound for the clicked key (octave 4)

    NotePicker();
    void paint (juce::Graphics&) override;
    void mouseDown (const juce::MouseEvent&) override;
    void mouseDrag (const juce::MouseEvent&) override;
    void mouseUp (const juce::MouseEvent&) override;
    void mouseMove (const juce::MouseEvent&) override;
    void mouseExit (const juce::MouseEvent&) override;

private:
    int keyAt (juce::Point<float> p) const;
    juce::Rectangle<float> keyBounds (int pitchClass) const;
    static bool isBlack (int pc) { return pc == 1 || pc == 3 || pc == 6 || pc == 8 || pc == 10; }

    int hover = -1, lastDragKey = -1, sounding = -1;
    bool dragTurnsOn = true;
};
} // namespace nf
