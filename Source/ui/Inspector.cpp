#include "Inspector.h"
#include "Theme.h"

namespace nf
{
Inspector::Inspector (NoteEditor& e) : editor (e)
{
    title.setFont (juce::FontOptions (18.0f, juce::Font::bold));
    detail.setFont (juce::FontOptions (12.0f));
    detail.setColour (juce::Label::textColourId, theme::textDim);
    addAndMakeVisible (title);
    addAndMakeVisible (detail);

    addField ("Pitch", -24.0, 24.0, 0.01, " st", 0.0,
              "How far the selected notes are moved up or down, in semitones (1.00 = one piano key).",
              [] (const Note& n) { return n.pitchOffset; }, [] (Note& n, float v) { n.pitchOffset = v; });
    addField ("Vibrato", 0.0, 300.0, 1.0, " %", 100.0,
              "Depth of the fast pitch wobble (vibrato). 100% = as sung, 0% = perfectly steady, 200% = twice as deep.",
              [] (const Note& n) { return n.modScale * 100.0f; }, [] (Note& n, float v) { n.modScale = v / 100.0f; });
    addField ("Drift", 0.0, 300.0, 1.0, " %", 100.0,
              "Slow pitch wandering within the note, like scoops into the note and sagging at the end. 0% = straight line.",
              [] (const Note& n) { return n.driftScale * 100.0f; }, [] (Note& n, float v) { n.driftScale = v / 100.0f; });
    addField ("Formant", -12.0, 12.0, 0.01, " st", 0.0,
              "Vocal character without changing the note. Up = smaller, brighter, more childlike; down = bigger, darker voice.",
              [] (const Note& n) { return n.formant; }, [] (Note& n, float v) { n.formant = v; });
    addField ("Gain", -48.0, 12.0, 0.1, " dB", 0.0,
              "Volume of the selected notes in decibels. -6 dB is about half as loud.",
              [] (const Note& n) { return n.gainDb; }, [] (Note& n, float v) { n.gainDb = v; });

    muteButton.setClickingTogglesState (false);
    muteButton.onClick = [this]
    {
        const auto& notes = editor.getNotes();
        const auto sel = editor.selectedIndices();
        const bool anyUnmuted = std::any_of (sel.begin(), sel.end(), [&] (int i) { return ! notes[(size_t) i].muted; });
        editor.applyToSelection ([anyUnmuted] (Note& n) { n.muted = anyUnmuted; });
        update();
    };
    resetButton.setTooltip ("Reset pitch, vibrato, drift, formant and gain of the selected notes (key: 0)");
    resetButton.onClick = [this] { editor.applyToSelection ([] (Note& n) { resetNote (n); }); update(); };
    addAndMakeVisible (muteButton);
    addAndMakeVisible (resetButton);
    update();
}

void Inspector::addField (const juce::String& name, double lo, double hi, double step, const juce::String& suffix,
                          double defaultValue, const juce::String& help,
                          std::function<float (const Note&)> get, std::function<void (Note&, float)> set)
{
    auto f = std::make_unique<Field>();
    f->label.setText (name, juce::dontSendNotification);
    f->label.setJustificationType (juce::Justification::centredRight);
    f->label.setColour (juce::Label::textColourId, theme::textDim);
    f->slider.setRange (lo, hi, step);
    f->slider.setTextValueSuffix (suffix);
    f->slider.setDefaultValue (defaultValue);
    f->slider.setHelp (name, help);
    f->label.setTooltip (f->slider.getTooltip());
    f->slider.setColour (juce::Slider::trackColourId, theme::accent.withAlpha (0.35f));
    f->get = std::move (get);
    f->set = std::move (set);
    auto* raw = f.get();
    f->slider.onDragStart = [this, raw] { raw->dragging = true; editor.beginGesture(); };
    f->slider.onDragEnd = [raw] { raw->dragging = false; };
    f->slider.onValueChange = [this, raw]
    {
        if (updating) return;
        const float v = (float) raw->slider.getValue();
        editor.applyToSelection ([raw, v] (Note& n) { raw->set (n, v); }, ! raw->dragging);
        update();
    };
    addAndMakeVisible (f->label);
    addAndMakeVisible (f->slider);
    fields.push_back (std::move (f));
}

void Inspector::update()
{
    const juce::ScopedValueSetter<bool> svs (updating, true);
    const auto sel = editor.selectedIndices();
    const auto& notes = editor.getNotes();
    const bool any = ! sel.empty();
    for (auto& f : fields)
    {
        f->slider.setEnabled (any);
        if (any) f->slider.setValue (f->get (notes[(size_t) sel.front()]), juce::dontSendNotification);
    }
    muteButton.setEnabled (any);
    resetButton.setEnabled (any);
    if (! any)
    {
        title.setText (notes.empty() ? "" : "-", juce::dontSendNotification);
        detail.setText (notes.empty() ? "" : juce::String (notes.size()) + " notes. Click a blob to select it.", juce::dontSendNotification);
        return;
    }
    const auto& n = notes[(size_t) sel.front()];
    const int cents = (int) std::lround ((n.center() - std::round (n.center())) * 100.0f);
    title.setText (juce::String (midiToName (n.center())) + (cents == 0 ? "" : juce::String (cents > 0 ? " +" : " ") + juce::String (cents) + "c"),
                   juce::dontSendNotification);
    detail.setText (sel.size() > 1 ? juce::String (sel.size()) + " notes selected"
                                   : "originally " + juce::String (midiToName (n.origCenter))
                                         + juce::String::formatted (" %+d c", (int) std::lround ((n.origCenter - std::round (n.origCenter)) * 100.0f))
                                         + (n.muted ? "  (muted)" : ""),
                    juce::dontSendNotification);
}

void Inspector::paint (juce::Graphics& g)
{
    g.fillAll (theme::panel);
    g.setColour (theme::border);
    g.drawHorizontalLine (0, 0.0f, (float) getWidth());
}

void Inspector::resized()
{
    auto r = getLocalBounds().reduced (10, 8);
    auto left = r.removeFromLeft (190);
    title.setBounds (left.removeFromTop (26));
    detail.setBounds (left);
    resetButton.setBounds (r.removeFromRight (64).withSizeKeepingCentre (64, 28));
    r.removeFromRight (6);
    muteButton.setBounds (r.removeFromRight (64).withSizeKeepingCentre (64, 28));
    r.removeFromRight (12);
    const int w = r.getWidth() / juce::jmax (1, (int) fields.size());
    for (auto& f : fields)
    {
        auto c = r.removeFromLeft (w).reduced (4, 0);
        f->label.setBounds (c.removeFromLeft (58).withSizeKeepingCentre (58, 24));
        f->slider.setBounds (c.withSizeKeepingCentre (c.getWidth(), 26));
    }
}
} // namespace nf
