#include "LiveView.h"
#include "Theme.h"

namespace nf
{
namespace
{
    bool isBlackKey (int m)
    {
        const int pc = ((m % 12) + 12) % 12;
        return pc == 1 || pc == 3 || pc == 6 || pc == 8 || pc == 10;
    }
    juce::String keyLabel (int m)
    {
        static const char* letters[12] = { "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B" };
        const int pc = ((m % 12) + 12) % 12;
        return pc == 0 ? juce::String ("C") + juce::String (m / 12 - 1) : juce::String (letters[pc]);
    }
}

LiveView::LiveView (juce::AudioProcessorValueTreeState& params, LiveTuner& t, std::function<Scale()> scale,
                    std::function<double()> latencyMs)
    : tuner (t), getScale (std::move (scale)), getLatencyMs (std::move (latencyMs))
{
    addKnob (params, "retune", "Retune Speed",
             "How fast a note is pulled onto the target pitch. 0 ms = instant, robotic 'T-Pain' effect. "
             "10-40 ms = tight but natural. 80 ms and up = gentle, keeps slides and vibrato.");
    addKnob (params, "amount", "Correction",
             "How much of the way to the target note the pitch is moved. 100% = fully in tune, "
             "50% = halfway, 0% = no correction (useful for formant or transpose only).");
    addKnob (params, "formant", "Formant",
             "Changes the character of the voice without changing the note. Up = smaller, brighter, chipmunk-like; "
             "down = bigger, deeper, darker. 0 = natural.");
    addKnob (params, "transpose", "Transpose",
             "Shifts the whole performance up or down by whole semitones after tuning. 12 = one octave.");
    addKnob (params, "mix", "Mix",
             "Blend between the original voice (0%) and the tuned voice (100%). In between can sound like a doubled voice.");

    rangeLabel.setText ("Voice range", juce::dontSendNotification);
    rangeLabel.setColour (juce::Label::textColourId, theme::textDim);
    rangeLabel.setJustificationType (juce::Justification::centredRight);
    if (auto* p = dynamic_cast<juce::AudioParameterChoice*> (params.getParameter ("range")))
        rangeBox.addItemList (p->choices, 1);
    rangeAttach = std::make_unique<juce::AudioProcessorValueTreeState::ComboBoxAttachment> (params, "range", rangeBox);
    const juce::String rangeHelp = "Voice range: the lowest note that gets tuned. Auto tunes everything from deep bass to soprano "
                                   "(about 70 ms delay, compensated by FL on playback). High/Mid only make sense if you need less "
                                   "delay while recording and never sing below them: notes under the range are NOT tuned.";
    rangeParam = params.getParameter ("range");
    useAutoButton.setColour (juce::TextButton::buttonColourId, theme::accent.withAlpha (0.85f));
    useAutoButton.setColour (juce::TextButton::textColourOffId, juce::Colours::black);
    useAutoButton.onClick = [this]
    {
        if (rangeParam != nullptr) rangeParam->setValueNotifyingHost (rangeParam->convertTo0to1 (3.0f));
        showBelowWarning = false;
        useAutoButton.setVisible (false);
        repaint();
    };
    addChildComponent (useAutoButton);

    engineLabel.setText ("Engine", juce::dontSendNotification);
    engineLabel.setColour (juce::Label::textColourId, theme::textDim);
    engineLabel.setJustificationType (juce::Justification::centredRight);
    if (auto* p = dynamic_cast<juce::AudioParameterChoice*> (params.getParameter ("liveEngine")))
        engineBox.addItemList (p->choices, 1);
    engineAttach = std::make_unique<juce::AudioProcessorValueTreeState::ComboBoxAttachment> (params, "liveEngine", engineBox);
    engineParam = params.getParameter ("liveEngine");
    const juce::String engineHelp =
        "Low latency (recording): use this while you record. You hear yourself tuned with only a few ms of delay, "
        "so it feels natural in your headphones. Formant is not available in this engine.\n"
        "High quality (mixing): cleaner sound and formant control, but ~70 ms of delay. FL lines it up on playback, "
        "but it feels late if you sing through it.";
    engineBox.setTooltip (engineHelp);
    engineLabel.setTooltip (engineHelp);
    addAndMakeVisible (engineLabel);
    addAndMakeVisible (engineBox);

    bypassButton.setClickingTogglesState (true);
    bypassButton.setColour (juce::TextButton::buttonOnColourId, theme::record);
    bypassButton.setColour (juce::TextButton::textColourOnId, juce::Colours::white);
    bypassButton.setTooltip ("Bypass: hear your voice without auto-tune, to compare. The delay stays the same, so switching "
                             "back and forth lines up exactly. The graph then shows only what you sing (grey).");
    bypassAttach = std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment> (params, "liveBypass", bypassButton);
    bypassButton.onStateChange = [this]
    {
        const auto t = bypassButton.getToggleState() ? juce::String ("BYPASSED") : juce::String ("BYPASS");
        if (bypassButton.getButtonText() != t) bypassButton.setButtonText (t);
    };
    bypassButton.onStateChange(); // correct label if a project opens already bypassed
    addAndMakeVisible (bypassButton);
    rangeBox.setTooltip (rangeHelp);
    rangeLabel.setTooltip (rangeHelp);
    addAndMakeVisible (rangeLabel);
    addAndMakeVisible (rangeBox);

    startTimerHz (30);
}

LiveView::~LiveView() { stopTimer(); }

void LiveView::addKnob (juce::AudioProcessorValueTreeState& params, const juce::String& id, const juce::String& name,
                        const juce::String& help)
{
    auto k = std::make_unique<Knob>();
    k->slider.setTextBoxStyle (juce::Slider::TextBoxBelow, false, 80, 20);
    k->slider.setHelp (name, help);
    k->label.setText (name, juce::dontSendNotification);
    k->label.setJustificationType (juce::Justification::centred);
    k->label.setColour (juce::Label::textColourId, theme::textDim);
    k->label.setTooltip (k->slider.getTooltip());
    k->attachment = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment> (params, id, k->slider);
    if (auto* p = params.getParameter (id))
        k->slider.setDefaultValue (p->convertFrom0to1 (p->getDefaultValue()));
    auto* label = &k->label;
    k->slider.onHoverChanged = [this, label] (ParamSlider* s, bool entered)
    {
        label->setColour (juce::Label::textColourId, entered ? theme::accent : theme::textDim);
        hoverName = entered ? s->displayName : juce::String();
        hoverHelp = entered ? s->helpText : juce::String();
        repaint (helpArea);
    };
    addAndMakeVisible (k->slider);
    addAndMakeVisible (k->label);
    knobs.push_back (std::move (k));
}

void LiveView::timerCallback()
{
    // flag notes sung below the selected range for 4 s after the last one
    const int below = tuner.notesBelowRange.load (std::memory_order_relaxed);
    const auto now = juce::Time::getMillisecondCounter();
    if (below != lastBelowCount)
    {
        lastBelowCount = below;
        belowSeenMs = now;
    }
    // formant only exists in the high-quality engine
    const bool lowLat = engineParam != nullptr && engineParam->getValue() < 0.5f;
    for (auto& k : knobs)
        if (k->slider.displayName == "Formant" && k->slider.isEnabled() == lowLat)
        {
            k->slider.setEnabled (! lowLat);
            k->label.setText (lowLat ? "Formant (mixing engine)" : "Formant", juce::dontSendNotification);
        }

    const bool warn = belowSeenMs != 0 && now - belowSeenMs < 4000
                      && rangeParam != nullptr && (int) rangeParam->convertFrom0to1 (rangeParam->getValue()) != 3;
    if (warn != showBelowWarning)
    {
        showBelowWarning = warn;
        useAutoButton.setVisible (warn);
        repaint (topRow);
    }
    const int w = tuner.historyWrite.load (std::memory_order_acquire);
    for (int i = 0; i < LiveTuner::historySize; ++i)
    {
        const int idx = (w + i) % LiveTuner::historySize;
        in[(size_t) i] = tuner.historyIn[(size_t) idx].load (std::memory_order_relaxed);
        out[(size_t) i] = tuner.historyOut[(size_t) idx].load (std::memory_order_relaxed);
    }
    // while bypassed the corrected (orange) values are 0: follow what is being sung instead
    const float last = out.back() > 0.0f ? out.back() : in.back();
    voicedNow = in.back() > 0.0f;
    if (last > 0.0f)
        centre += (last - centre) * 0.08f;
    if (voicedNow)
    {
        const float target = getScale().snap (in.back());
        meterCents += (juce::jlimit (-50.0f, 50.0f, (in.back() - target) * 100.0f) - meterCents) * 0.35f;
    }
    repaint (graphArea.getUnion (keyArea).getUnion (meterArea).getUnion (topRow));
}

void LiveView::paint (juce::Graphics& g)
{
    g.fillAll (theme::bg);
    paintGraph (g);
    paintMeter (g);

    if (showBelowWarning)
    {
        auto w = topRow.toFloat().withTrimmedLeft (760.0f).withTrimmedRight (90.0f + 8.0f);
        g.setColour (theme::record.withAlpha (0.18f));
        g.fillRoundedRectangle (w, 5.0f);
        g.setColour (theme::record.brighter (0.4f));
        g.setFont (juce::FontOptions (13.0f, juce::Font::bold));
        g.drawText ("You're singing below this voice range - those notes are not being tuned.", w.reduced (10.0f, 0.0f),
                    juce::Justification::centredLeft);
    }
    else
    {
        // latency readout (the warning takes its place while it is shown)
        g.setColour (theme::textDim);
        g.setFont (juce::FontOptions (12.0f));
        juce::String text = "Delay " + juce::String (juce::roundToInt (getLatencyMs())) + " ms";
        bool bad = false;
        if (getDiagnostics)
        {
            float now = 0, peak = 0; int over = 0;
            getDiagnostics (now, peak, over);
            text << "   |   NoteForge CPU " << juce::roundToInt (now * 100.0f) << "% (peak " << juce::roundToInt (peak * 100.0f) << "%)";
            if (over > 0)
                text << "   |   overloaded " << over << "x - raise FL's buffer size";
            bad = over > 0;
        }
        g.setColour (bad ? theme::record.brighter (0.3f) : theme::textDim);
        g.drawText (text, topRow.withTrimmedLeft (760), juce::Justification::centredRight);
    }

    // help strip: explains the hovered control
    auto h = helpArea.toFloat();
    g.setColour (theme::panel);
    g.fillRoundedRectangle (h, 6.0f);
    if (hoverName.isNotEmpty())
    {
        g.setColour (theme::accent);
        g.fillRoundedRectangle (h.removeFromLeft (4.0f), 2.0f);
        g.setFont (juce::FontOptions (13.0f, juce::Font::bold));
        g.drawText (hoverName, h.removeFromLeft (120.0f).reduced (10.0f, 0.0f), juce::Justification::centredLeft);
        g.setColour (theme::text);
        g.setFont (juce::FontOptions (13.0f));
        g.drawFittedText (hoverHelp, h.reduced (6.0f, 2.0f).toNearestInt(), juce::Justification::centredLeft, 2);
    }
    else
    {
        g.setColour (theme::textDim);
        g.setFont (juce::FontOptions (12.5f));
        g.drawText ("Hover a knob to see what it does  |  right-click a knob to reset it or type a value  |  "
                    "click a piano key to hear it",
                    h.reduced (12.0f, 0.0f), juce::Justification::centredLeft);
    }
}

void LiveView::paintGraph (juce::Graphics& g)
{
    const auto scale = getScale();
    const auto r = graphArea.toFloat();
    const auto keys = keyArea.toFloat();
    g.setColour (theme::panel);
    g.fillRoundedRectangle (keys.getUnion (r), 6.0f);

    const float span = viewSpan;
    const float top = centre + span * 0.5f;
    viewTop = top;
    const float rowH = r.getHeight() / span;
    auto yFor = [&] (float m) { return r.getY() + (top - m) / span * r.getHeight(); };
    const int sung = voicedNow ? (int) std::lround (in.back()) : -1000;
    const int target = (voicedNow && out.back() > 0.0f) ? (int) std::lround (out.back()) : -1000; // none while bypassed

    juce::Graphics::ScopedSaveState ss (g);
    g.reduceClipRegion (keyArea.getUnion (graphArea));

    for (int m = (int) std::floor (top - span) - 1; m <= (int) std::ceil (top) + 1; ++m)
    {
        const float y0 = yFor ((float) m + 0.5f), y1 = yFor ((float) m - 0.5f);
        const bool inScale = scale.contains (m);

        // lane
        auto lane = juce::Rectangle<float> (r.getX(), y0, r.getWidth(), y1 - y0);
        g.setColour (m == target ? theme::accent.withAlpha (0.16f) : inScale ? theme::rowIn.brighter (0.06f) : theme::rowOut);
        g.fillRect (lane);
        g.setColour (theme::gridLine);
        g.drawHorizontalLine ((int) y1, r.getX(), r.getRight());

        // piano key
        auto key = juce::Rectangle<float> (keys.getX(), y0, keys.getWidth(), y1 - y0).reduced (0.0f, 0.5f);
        const bool black = isBlackKey (m);
        juce::Colour kc = black ? juce::Colour (0xff2b2e35) : juce::Colour (0xffd7dbe2);
        if (! inScale) kc = kc.interpolatedWith (theme::bg, 0.5f);
        if (m == sung) kc = theme::accent;
        if (m == sounding) kc = theme::accent.brighter (0.3f);
        g.setColour (kc);
        g.fillRect (key);
        if (m == sung)
        {
            for (int i = 1; i <= 3; ++i)
            {
                g.setColour (theme::accent.withAlpha (0.12f));
                g.fillRect (key.expanded ((float) i * 2.0f, 0.0f).withRight (keys.getRight()));
            }
        }
        if (! black || m == sung)
            g.setColour (m == sung ? juce::Colours::black : juce::Colour (0xff2b2e35));
        else
            g.setColour (theme::text.withAlpha (inScale ? 0.85f : 0.4f));
        g.setFont (juce::FontOptions (juce::jlimit (9.0f, 14.0f, rowH * 0.62f), m == sung ? juce::Font::bold : juce::Font::plain));
        g.drawText (keyLabel (m), key.reduced (6.0f, 0.0f), juce::Justification::centredRight, false);
        if (inScale && scale.mask != allNotesMask)
        {
            g.setColour (theme::accent);
            g.fillEllipse (key.getX() + 5.0f, key.getCentreY() - 3.0f, 6.0f, 6.0f);
        }
    }
    g.setColour (theme::border);
    g.drawVerticalLine ((int) keys.getRight(), keys.getY(), keys.getBottom());

    auto trace = [&] (const std::array<float, LiveTuner::historySize>& v, juce::Colour c, float width)
    {
        juce::Path p;
        bool open = false;
        for (int i = 0; i < LiveTuner::historySize; ++i)
        {
            if (v[(size_t) i] <= 0.0f) { open = false; continue; }
            const float x = r.getX() + 6.0f + (r.getWidth() - 20.0f) * (float) i / (float) (LiveTuner::historySize - 1);
            const float y = yFor (v[(size_t) i]);
            if (! open) p.startNewSubPath (x, y); else p.lineTo (x, y);
            open = true;
        }
        g.setColour (c);
        g.strokePath (p, juce::PathStrokeType (width, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
    };
    const bool bypassed = bypassButton.getToggleState();
    trace (in, bypassed ? theme::text.withAlpha (0.9f) : theme::textDim.withAlpha (0.85f), bypassed ? 2.0f : 1.4f);
    trace (out, theme::accent, 2.4f); // corrected pitch: no values are recorded while bypassed

    if (voicedNow)
    {
        const float x = r.getRight() - 14.0f;
        g.setColour (bypassed ? theme::text : theme::textDim);
        g.fillEllipse (juce::Rectangle<float> (bypassed ? 10.0f : 7.0f, bypassed ? 10.0f : 7.0f).withCentre ({ x, yFor (in.back()) }));
        if (! bypassed && out.back() > 0.0f)
        {
            g.setColour (theme::accent);
            g.fillEllipse (juce::Rectangle<float> (11.0f, 11.0f).withCentre ({ x, yFor (out.back()) }));
        }
    }
    if (bypassed)
    {
        auto badge = r.reduced (10.0f).removeFromTop (26.0f).removeFromLeft (330.0f);
        g.setColour (theme::record.withAlpha (0.85f));
        g.fillRoundedRectangle (badge, 5.0f);
        g.setColour (juce::Colours::white);
        g.setFont (juce::FontOptions (13.0f, juce::Font::bold));
        g.drawText ("BYPASSED - you hear your voice without tuning", badge, juce::Justification::centred);
    }

    g.setColour (theme::textDim);
    g.setFont (juce::FontOptions (11.5f));
    g.drawText ("grey = what you sing   orange = tuned output", r.reduced (10.0f, 6.0f).removeFromBottom (16.0f),
                juce::Justification::centredLeft);
}

void LiveView::paintMeter (juce::Graphics& g)
{
    auto m = meterArea.toFloat();
    g.setColour (theme::panel);
    g.fillRoundedRectangle (m, 6.0f);
    m.reduce (12.0f, 10.0f);

    g.setColour (voicedNow ? theme::text : theme::textDim);
    g.setFont (juce::FontOptions (40.0f, juce::Font::bold));
    const float shown = out.back() > 0.0f ? out.back() : in.back();
    const juce::String noteName = voicedNow ? juce::String (midiToName (shown)) : juce::String ("--");
    g.drawText (noteName, m.removeFromTop (52.0f), juce::Justification::centred);
    g.setFont (juce::FontOptions (12.0f));
    g.setColour (theme::textDim);
    g.drawText (voicedNow ? "you're singing " + juce::String (midiToName (in.back()))
                                + juce::String::formatted (" %+d c", (int) std::lround (meterCents))
                          : juce::String ("waiting for a voice..."),
                m.removeFromTop (18.0f), juce::Justification::centred);
    m.removeFromTop (8.0f);

    // tuning meter: flat (left) .. in tune (centre) .. sharp (right)
    auto bar = m.removeFromTop (14.0f);
    g.setGradientFill (juce::ColourGradient (juce::Colour (0xff3c78d8), bar.getX(), 0.0f, juce::Colour (0xffe5484d), bar.getRight(), 0.0f, false));
    g.fillRoundedRectangle (bar, 7.0f);
    g.setColour (juce::Colour (0xff3ecf8e));
    g.fillRoundedRectangle (bar.withSizeKeepingCentre (bar.getWidth() * 0.16f, bar.getHeight()), 7.0f);
    if (voicedNow)
    {
        const float x = bar.getX() + bar.getWidth() * (0.5f + meterCents / 100.0f);
        g.setColour (juce::Colours::white);
        g.fillRoundedRectangle (juce::Rectangle<float> (4.0f, bar.getHeight() + 8.0f).withCentre ({ x, bar.getCentreY() }), 2.0f);
    }
    g.setColour (theme::textDim);
    g.setFont (juce::FontOptions (11.0f));
    auto labels = m.removeFromTop (16.0f);
    g.drawText ("flat", labels, juce::Justification::centredLeft);
    g.drawText ("in tune", labels, juce::Justification::centred);
    g.drawText ("sharp", labels, juce::Justification::centredRight);
}

int LiveView::keyMidiAt (float y) const
{
    const auto r = graphArea.toFloat();
    return (int) std::lround (viewTop - (y - r.getY()) / r.getHeight() * viewSpan);
}

void LiveView::mouseDown (const juce::MouseEvent& e)
{
    if (topRow.withTrimmedLeft (760).contains (e.getPosition()) && resetDiagnostics)
    {
        resetDiagnostics(); // click the CPU readout to reset the peak / overload count
        repaint (topRow);
        return;
    }
    if (! keyArea.contains (e.getPosition()) || ! playNote)
        return;
    sounding = keyMidiAt (e.position.y);
    playNote (sounding, true);
    repaint (keyArea);
}

void LiveView::mouseDrag (const juce::MouseEvent& e)
{
    if (sounding < 0 || ! playNote)
        return;
    const int m = keyMidiAt (e.position.y);
    if (m != sounding)
    {
        playNote (sounding, false);
        playNote (m, true);
        sounding = m;
        repaint (keyArea);
    }
}

void LiveView::mouseUp (const juce::MouseEvent&)
{
    if (sounding >= 0 && playNote)
        playNote (sounding, false);
    sounding = -1;
    repaint (keyArea);
}

void LiveView::resized()
{
    auto r = getLocalBounds().reduced (14);
    topRow = r.removeFromTop (30);
    {
        auto t = topRow;
        rangeLabel.setBounds (t.removeFromLeft (84));
        t.removeFromLeft (6);
        rangeBox.setBounds (t.removeFromLeft (220).reduced (0, 2));
        t.removeFromLeft (14);
        engineLabel.setBounds (t.removeFromLeft (54));
        t.removeFromLeft (6);
        engineBox.setBounds (t.removeFromLeft (236).reduced (0, 2));
        t.removeFromLeft (14);
        bypassButton.setBounds (t.removeFromLeft (104).reduced (0, 1));
        useAutoButton.setBounds (t.removeFromRight (90).reduced (0, 2));
    }
    r.removeFromTop (8);
    helpArea = r.removeFromBottom (40);
    r.removeFromBottom (8);
    auto knobRow = r.removeFromBottom (130);
    r.removeFromBottom (10);

    meterArea = r.removeFromRight (220).removeFromTop (150);
    r.removeFromRight (10);
    keyArea = r.removeFromLeft (64);
    graphArea = r;

    const int w = knobRow.getWidth() / (int) knobs.size();
    for (auto& k : knobs)
    {
        auto c = knobRow.removeFromLeft (w).reduced (8, 0);
        k->label.setBounds (c.removeFromTop (20));
        k->slider.setBounds (c);
    }
}
} // namespace nf
