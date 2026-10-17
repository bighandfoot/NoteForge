#pragma once
#include <juce_gui_basics/juce_gui_basics.h>

namespace nf::theme
{
inline const juce::Colour bg        { 0xff15171c };
inline const juce::Colour panel     { 0xff1d2027 };
inline const juce::Colour panelHi   { 0xff262a33 };
inline const juce::Colour border    { 0xff31353f };
inline const juce::Colour text      { 0xffd9dde5 };
inline const juce::Colour textDim   { 0xff8a909c };
inline const juce::Colour rowIn     { 0xff20232a };
inline const juce::Colour rowOut    { 0xff191b20 };
inline const juce::Colour gridLine  { 0xff2a2d35 };
inline const juce::Colour barLine   { 0xff3a3f4a };
inline const juce::Colour blob      { 0xff2fb8c9 };
inline const juce::Colour blobSel   { 0xffff9f43 };
inline const juce::Colour blobMuted { 0xff5a5f6a };
inline const juce::Colour pitchLine { 0xfff4f6fa };
inline const juce::Colour accent    { 0xffff9f43 };
inline const juce::Colour record    { 0xffe5484d };
inline const juce::Colour formant   { 0xffb287ff };
inline const juce::Colour playhead  { 0xffff5a5f };

class LookAndFeel : public juce::LookAndFeel_V4
{
public:
    LookAndFeel()
    {
        setColourScheme ({ panel, bg, panelHi, border, text, panelHi, text, accent, text });
        setColour (juce::TextButton::buttonColourId, panelHi);
        setColour (juce::TextButton::buttonOnColourId, accent.withAlpha (0.85f));
        setColour (juce::TextButton::textColourOffId, text);
        setColour (juce::TextButton::textColourOnId, juce::Colours::black);
        setColour (juce::ComboBox::backgroundColourId, panelHi);
        setColour (juce::ComboBox::outlineColourId, border);
        setColour (juce::ComboBox::arrowColourId, textDim);
        setColour (juce::PopupMenu::backgroundColourId, panel);
        setColour (juce::PopupMenu::highlightedBackgroundColourId, accent.withAlpha (0.8f));
        setColour (juce::Slider::thumbColourId, accent);
        setColour (juce::Slider::trackColourId, accent.withAlpha (0.6f));
        setColour (juce::Slider::backgroundColourId, panelHi);
        setColour (juce::Slider::rotarySliderFillColourId, accent);
        setColour (juce::Slider::rotarySliderOutlineColourId, panelHi);
        setColour (juce::Slider::textBoxOutlineColourId, juce::Colours::transparentBlack);
        setColour (juce::Slider::textBoxBackgroundColourId, bg);
        setColour (juce::Label::textColourId, text);
        setColour (juce::ScrollBar::thumbColourId, juce::Colour (0xff3d424d));
    }

    void drawButtonBackground (juce::Graphics& g, juce::Button& b, const juce::Colour& base,
                               bool over, bool down) override
    {
        auto r = b.getLocalBounds().toFloat().reduced (0.5f);
        auto c = b.getToggleState() ? b.findColour (juce::TextButton::buttonOnColourId) : base;
        if (down) c = c.brighter (0.15f);
        else if (over) c = c.brighter (0.07f);
        g.setColour (c);
        g.fillRoundedRectangle (r, 4.0f);
        g.setColour (border);
        g.drawRoundedRectangle (r, 4.0f, 1.0f);
    }

    juce::Font getTextButtonFont (juce::TextButton&, int h) override
    {
        return juce::Font (juce::FontOptions (juce::jmin (13.0f, (float) h * 0.55f)));
    }

    void drawRotarySlider (juce::Graphics& g, int x, int y, int w, int h, float pos,
                           float startAngle, float endAngle, juce::Slider& s) override
    {
        const auto bounds = juce::Rectangle<int> (x, y, w, h).toFloat().reduced (6.0f);
        const float radius = juce::jmin (bounds.getWidth(), bounds.getHeight()) * 0.5f;
        const auto c = bounds.getCentre();
        const bool hot = s.isMouseOverOrDragging() && s.isEnabled();

        if (hot)
        {
            // soft glow behind the hovered knob
            for (int i = 4; i >= 1; --i)
            {
                g.setColour (accent.withAlpha (0.05f * (float) i));
                g.fillEllipse (juce::Rectangle<float> (radius * 2.0f, radius * 2.0f).withCentre (c).expanded ((float) (5 - i) * 3.0f));
            }
        }
        g.setColour (panelHi);
        g.fillEllipse (juce::Rectangle<float> (radius * 1.5f, radius * 1.5f).withCentre (c));

        const float track = juce::jmax (3.0f, radius * 0.16f);
        const float arcR = radius - track * 0.5f;
        juce::Path trackPath;
        trackPath.addCentredArc (c.x, c.y, arcR, arcR, 0.0f, startAngle, endAngle, true);
        g.setColour (juce::Colour (0xff2e323b));
        g.strokePath (trackPath, juce::PathStrokeType (track, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));

        // value arc starts at the default/centre for bipolar ranges
        const bool bipolar = s.getMinimum() < 0.0 && s.getMaximum() > 0.0;
        const float from = bipolar ? startAngle + (float) s.valueToProportionOfLength (0.0) * (endAngle - startAngle) : startAngle;
        const float to = startAngle + pos * (endAngle - startAngle);
        juce::Path val;
        val.addCentredArc (c.x, c.y, arcR, arcR, 0.0f, juce::jmin (from, to), juce::jmax (from, to), true);
        g.setColour (hot ? accent.brighter (0.2f) : accent);
        g.strokePath (val, juce::PathStrokeType (track, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));

        const juce::Point<float> tip (c.x + std::sin (to) * radius * 0.6f, c.y - std::cos (to) * radius * 0.6f);
        g.setColour (text);
        g.drawLine ({ c, tip }, 2.5f);
    }
};
} // namespace nf::theme
