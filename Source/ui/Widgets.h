#pragma once
#include <juce_gui_basics/juce_gui_basics.h>
#include "Theme.h"

namespace nf
{
// Slider/knob with a right-click menu (reset to default, type in a value), a hover highlight,
// and a help text that is shown as a tooltip and reported to an optional help strip on hover.
class ParamSlider : public juce::Slider
{
public:
    using juce::Slider::Slider;

    juce::String displayName, helpText;
    std::function<void (ParamSlider*, bool)> onHoverChanged; // (slider, entered)

    void setDefaultValue (double v)
    {
        defaultValue = v;
        setDoubleClickReturnValue (true, v);
    }
    void setHelp (const juce::String& name, const juce::String& help)
    {
        displayName = name;
        helpText = help;
        setTooltip (name + ": " + help + "\n(Right-click: reset or type a value. Double-click: reset.)");
    }

    void mouseDown (const juce::MouseEvent& e) override
    {
        if (! e.mods.isPopupMenu())
        {
            juce::Slider::mouseDown (e);
            return;
        }
        juce::PopupMenu m;
        m.addSectionHeader (displayName.isNotEmpty() ? displayName : getName());
        m.addItem (1, "Reset to default (" + getTextFromValue (defaultValue) + ")");
        m.addItem (2, "Set value...");
        m.addSeparator();
        m.addItem (3, "Copy value");
        m.addItem (4, "Paste value", juce::SystemClipboard::getTextFromClipboard().trim().containsAnyOf ("0123456789"));
        juce::Component::SafePointer<ParamSlider> safe (this);
        m.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (this), [safe] (int r)
        {
            if (safe == nullptr) return;
            if (r == 1) safe->setValue (safe->defaultValue, juce::sendNotificationSync);
            if (r == 2) safe->askForValue();
            if (r == 3) juce::SystemClipboard::copyTextToClipboard (safe->getTextFromValue (safe->getValue()));
            if (r == 4) safe->setValue (safe->getValueFromText (juce::SystemClipboard::getTextFromClipboard()), juce::sendNotificationSync);
        });
    }

    void mouseEnter (const juce::MouseEvent& e) override
    {
        juce::Slider::mouseEnter (e);
        if (onHoverChanged) onHoverChanged (this, true);
        repaint();
    }
    void mouseExit (const juce::MouseEvent& e) override
    {
        juce::Slider::mouseExit (e);
        if (onHoverChanged) onHoverChanged (this, false);
        repaint();
    }

    void askForValue()
    {
        auto* w = new juce::AlertWindow ("Set value", displayName.isNotEmpty() ? displayName : getName(),
                                         juce::MessageBoxIconType::NoIcon, this);
        w->addTextEditor ("value", getTextFromValue (getValue()));
        w->addButton ("OK", 1, juce::KeyPress (juce::KeyPress::returnKey));
        w->addButton ("Cancel", 0, juce::KeyPress (juce::KeyPress::escapeKey));
        if (auto* ed = w->getTextEditor ("value"))
            ed->selectAll();
        juce::Component::SafePointer<ParamSlider> safe (this);
        w->enterModalState (true, juce::ModalCallbackFunction::create ([safe, w] (int r)
        {
            if (r == 1 && safe != nullptr)
                safe->setValue (safe->getValueFromText (w->getTextEditorContents ("value")), juce::sendNotificationSync);
        }), true);
    }

private:
    double defaultValue = 0.0;
};

// Small transport button with a drawn icon: back-to-marker-start, or play/stop.
class TransportButton : public juce::Button
{
public:
    enum class Kind { toStart, playStop };
    explicit TransportButton (Kind k) : juce::Button ({}), kind (k) {}
    bool showStop = false;

    void paintButton (juce::Graphics& g, bool over, bool down) override
    {
        auto r = getLocalBounds().toFloat().reduced (0.5f);
        auto c = showStop ? juce::Colours::limegreen.darker (0.3f) : theme::panelHi;
        if (down) c = c.brighter (0.15f); else if (over) c = c.brighter (0.07f);
        g.setColour (c);
        g.fillRoundedRectangle (r, 4.0f);
        g.setColour (theme::border);
        g.drawRoundedRectangle (r, 4.0f, 1.0f);

        const auto icon = r.withSizeKeepingCentre (12.0f, 12.0f);
        g.setColour (showStop ? juce::Colours::black : theme::text);
        juce::Path p;
        if (kind == Kind::toStart)
        {
            p.addRectangle (icon.getX(), icon.getY(), 2.5f, icon.getHeight());
            p.addTriangle (icon.getRight(), icon.getY(), icon.getRight(), icon.getBottom(), icon.getX() + 2.5f, icon.getCentreY());
        }
        else if (showStop)
            p.addRectangle (icon.reduced (1.0f));
        else
            p.addTriangle (icon.getX() + 1.0f, icon.getY(), icon.getX() + 1.0f, icon.getBottom(), icon.getRight(), icon.getCentreY());
        g.fillPath (p);
    }

private:
    Kind kind;
};
} // namespace nf
