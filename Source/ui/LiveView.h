#pragma once
#include <juce_audio_processors/juce_audio_processors.h>
#include "dsp/LiveTuner.h"
#include "Widgets.h"

namespace nf
{
// Live auto-tune page: a piano-roll pitch graph (labelled keyboard, the note you're singing lit up),
// a tuning meter, correction knobs and a help strip that explains whatever control you hover.
class LiveView : public juce::Component, private juce::Timer
{
public:
    LiveView (juce::AudioProcessorValueTreeState& params, LiveTuner& tuner,
              std::function<Scale()> scale, std::function<double()> latencyMs);
    ~LiveView() override;
    void paint (juce::Graphics&) override;
    void resized() override;
    void mouseDown (const juce::MouseEvent&) override;
    void mouseDrag (const juce::MouseEvent&) override;
    void mouseUp (const juce::MouseEvent&) override;

    std::function<void (int midi, bool on)> playNote; // clicked keys on the keyboard strip
    // live CPU meter: current and peak share of real time NoteForge uses (0..1), and windows over 70 %
    std::function<void (float& now, float& peak, int& overloads)> getDiagnostics;
    std::function<void()> resetDiagnostics;

private:
    int keyMidiAt (float y) const;
    void timerCallback() override;
    void paintGraph (juce::Graphics&);
    void paintMeter (juce::Graphics&);

    struct Knob
    {
        ParamSlider slider { juce::Slider::RotaryHorizontalVerticalDrag, juce::Slider::TextBoxBelow };
        juce::Label label;
        std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> attachment;
    };
    void addKnob (juce::AudioProcessorValueTreeState& params, const juce::String& id, const juce::String& name,
                  const juce::String& help);

    LiveTuner& tuner;
    std::function<Scale()> getScale;
    std::function<double()> getLatencyMs;
    std::vector<std::unique_ptr<Knob>> knobs;
    juce::ComboBox rangeBox, engineBox;
    juce::Label rangeLabel, engineLabel;
    std::unique_ptr<juce::AudioProcessorValueTreeState::ComboBoxAttachment> engineAttach;
    juce::RangedAudioParameter* engineParam = nullptr;
    juce::TextButton bypassButton { "BYPASS" };
    std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment> bypassAttach;
    std::unique_ptr<juce::AudioProcessorValueTreeState::ComboBoxAttachment> rangeAttach;
    juce::String hoverName, hoverHelp;

    // warning when the singer goes below the selected voice range
    juce::RangedAudioParameter* rangeParam = nullptr;
    juce::TextButton useAutoButton { "Use Auto" };
    int lastBelowCount = 0;
    juce::uint32 belowSeenMs = 0;
    bool showBelowWarning = false;

    juce::Rectangle<int> topRow, graphArea, keyArea, meterArea, helpArea;
    std::array<float, LiveTuner::historySize> in {}, out {};
    float centre = 60.0f;
    float meterCents = 0.0f;
    bool voicedNow = false;
    float viewTop = 67.5f, viewSpan = 15.0f; // pitch range shown in the graph (updated on paint)
    int sounding = -1;
};
} // namespace nf
