#pragma once
#include "NoteEditor.h"
#include "Widgets.h"

namespace nf
{
// Bottom bar: numeric view/edit of the selected notes' parameters.
class Inspector : public juce::Component
{
public:
    explicit Inspector (NoteEditor& editor);
    void update();
    void paint (juce::Graphics&) override;
    void resized() override;

private:
    struct Field
    {
        juce::Label label;
        ParamSlider slider { juce::Slider::LinearBar, juce::Slider::TextBoxRight };
        std::function<float (const Note&)> get;
        std::function<void (Note&, float)> set;
        bool dragging = false;
    };
    void addField (const juce::String& name, double lo, double hi, double step, const juce::String& suffix,
                   double defaultValue, const juce::String& help,
                   std::function<float (const Note&)> get, std::function<void (Note&, float)> set);

    NoteEditor& editor;
    juce::Label title, detail;
    std::vector<std::unique_ptr<Field>> fields;
    juce::TextButton muteButton { "Mute" }, resetButton { "Reset" };
    bool updating = false;
};
} // namespace nf
