#include "NotePicker.h"
#include "Theme.h"

namespace nf
{
namespace
{
    const int whiteIndex[12] = { 0, 0, 1, 1, 2, 3, 3, 4, 4, 5, 5, 6 }; // white key a pitch class sits on / after
    const char* names[12] = { "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B" };
}

NotePicker::NotePicker()
{
    setTooltip ("Click keys to choose which notes NoteForge tunes to. Lit keys are allowed. "
                "Drag across keys to toggle several. Right-click: all notes on.");
    setRepaintsOnMouseActivity (true);
}

juce::Rectangle<float> NotePicker::keyBounds (int pc) const
{
    const auto r = getLocalBounds().toFloat().reduced (1.0f);
    const float w = r.getWidth() / 7.0f;
    if (! isBlack (pc))
        return { r.getX() + w * (float) whiteIndex[pc], r.getY(), w, r.getHeight() };
    const float bw = w * 0.62f;
    return { r.getX() + w * (float) (whiteIndex[pc] + 1) - bw * 0.5f, r.getY(), bw, r.getHeight() * 0.6f };
}

int NotePicker::keyAt (juce::Point<float> p) const
{
    for (int pc = 0; pc < 12; ++pc)
        if (isBlack (pc) && keyBounds (pc).contains (p)) return pc;
    for (int pc = 0; pc < 12; ++pc)
        if (! isBlack (pc) && keyBounds (pc).contains (p)) return pc;
    return -1;
}

void NotePicker::paint (juce::Graphics& g)
{
    const unsigned mask = getMask ? getMask() : 0xFFFu;
    const int root = getRoot ? getRoot() : -1;
    const float fontH = juce::jlimit (8.0f, 13.0f, (float) getHeight() * 0.28f);
    g.setFont (juce::FontOptions (fontH));

    auto drawKey = [&] (int pc)
    {
        const auto b = keyBounds (pc);
        const bool on = (mask >> pc) & 1u;
        const bool black = isBlack (pc);
        juce::Colour c = black ? juce::Colour (0xff23262c) : juce::Colour (0xff6d727c);
        if (on) c = black ? theme::accent.darker (0.35f) : theme::accent;
        if (pc == hover) c = c.brighter (0.25f);
        g.setColour (c);
        g.fillRoundedRectangle (b.reduced (black ? 0.0f : 0.8f, 0.0f), 2.5f);
        g.setColour (theme::bg);
        g.drawRoundedRectangle (b.reduced (black ? 0.0f : 0.8f, 0.0f), 2.5f, 1.0f);

        const auto labelArea = black ? b.withTrimmedTop (b.getHeight() * 0.45f) : b.withTrimmedTop (b.getHeight() * 0.62f);
        g.setColour (on ? juce::Colours::black.withAlpha (0.85f) : theme::text.withAlpha (0.55f));
        if (! black || getHeight() >= 40)
            g.drawText (names[pc], labelArea, juce::Justification::centred, false);
        if (pc == root)
        {
            const float d = juce::jmax (3.0f, b.getWidth() * 0.16f);
            g.setColour (on ? juce::Colours::black : theme::accent);
            g.fillEllipse (b.getCentreX() - d * 0.5f, b.getY() + d * 0.8f, d, d);
        }
    };
    for (int pc = 0; pc < 12; ++pc) if (! isBlack (pc)) drawKey (pc);
    for (int pc = 0; pc < 12; ++pc) if (isBlack (pc)) drawKey (pc);
}

void NotePicker::mouseDown (const juce::MouseEvent& e)
{
    if (! getMask || ! setMask)
        return;
    if (e.mods.isPopupMenu())
    {
        setMask (0xFFFu);
        repaint();
        return;
    }
    const int k = keyAt (e.position);
    if (k < 0) return;
    const unsigned mask = getMask();
    dragTurnsOn = ! ((mask >> k) & 1u);
    unsigned next = dragTurnsOn ? (mask | (1u << k)) : (mask & ~(1u << k));
    if (next == 0) next = 1u << k; // never leave zero notes selected
    setMask (next);
    lastDragKey = k;
    if (playNote) { playNote (60 + k, true); sounding = 60 + k; }
    repaint();
}

void NotePicker::mouseDrag (const juce::MouseEvent& e)
{
    if (! getMask || ! setMask || e.mods.isPopupMenu())
        return;
    const int k = keyAt (e.position);
    if (k < 0 || k == lastDragKey) return;
    lastDragKey = k;
    unsigned next = dragTurnsOn ? (getMask() | (1u << k)) : (getMask() & ~(1u << k));
    if (next != 0) setMask (next);
    if (playNote)
    {
        if (sounding >= 0) playNote (sounding, false);
        playNote (60 + k, true);
        sounding = 60 + k;
    }
    hover = k;
    repaint();
}

void NotePicker::mouseUp (const juce::MouseEvent&)
{
    lastDragKey = -1;
    if (playNote && sounding >= 0) playNote (sounding, false);
    sounding = -1;
}

void NotePicker::mouseMove (const juce::MouseEvent& e)
{
    const int k = keyAt (e.position);
    if (k != hover) { hover = k; repaint(); }
}

void NotePicker::mouseExit (const juce::MouseEvent&) { hover = -1; repaint(); }
} // namespace nf
