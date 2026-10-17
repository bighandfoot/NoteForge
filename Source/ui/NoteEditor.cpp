#include "NoteEditor.h"
#include "Theme.h"

namespace nf
{
namespace
{
    bool isBlackKey (int midi)
    {
        const int pc = ((midi % 12) + 12) % 12;
        return pc == 1 || pc == 3 || pc == 6 || pc == 8 || pc == 10;
    }

    juce::String centsText (float midi)
    {
        const int cents = (int) std::lround ((midi - std::round (midi)) * 100.0f);
        if (cents == 0) return {};
        return (cents > 0 ? "+" : "") + juce::String (cents);
    }
}

NoteEditor::NoteEditor (Engine& e, EditorState& s) : engine (e), state (s)
{
    setWantsKeyboardFocus (true);
    setOpaque (true);
    addAndMakeVisible (hScroll);
    addAndMakeVisible (vScroll);
    hScroll.addListener (this);
    vScroll.addListener (this);
    hScroll.setAutoHide (false);
    vScroll.setAutoHide (false);
    refreshFromEngine();
    startTimerHz (30);
}

NoteEditor::~NoteEditor()
{
    hScroll.removeListener (this);
    vScroll.removeListener (this);
}

// ============================================================ geometry
juce::Rectangle<int> NoteEditor::gridArea() const
{
    return getLocalBounds().withTrimmedLeft (keyW).withTrimmedTop (rulerH).withTrimmedRight (scrollW).withTrimmedBottom (scrollW);
}

double NoteEditor::sr() const { return analysis ? analysis->sampleRate : 44100.0; }
float NoteEditor::xForSample (double s) const { return (float) gridArea().getX() + (float) ((s - viewStart) / samplesPerPx); }
double NoteEditor::sampleForX (float x) const { return viewStart + (double) (x - (float) gridArea().getX()) * samplesPerPx; }
float NoteEditor::yForMidi (float m) const { return (float) gridArea().getY() + (topMidi - m) * pxPerSemi; }
float NoteEditor::midiForY (float y) const { return topMidi - (y - (float) gridArea().getY()) / pxPerSemi; }

double NoteEditor::noteFrameToOut (const Note& n, double frame) const
{
    const int hop = analysis->hop;
    const double inS = n.inStart (hop), inE = n.inEnd (hop);
    return n.outStart + (frame * hop - inS) * (n.outEnd - n.outStart) / juce::jmax (1.0, inE - inS);
}

juce::Rectangle<float> NoteEditor::noteBounds (const Note& n) const
{
    const float x0 = xForSample (n.outStart), x1 = xForSample (n.outEnd);
    const float cy = yForMidi (n.center());
    const float h = juce::jmax (pxPerSemi * 0.5f, 5.0f);
    return { x0, cy - h, juce::jmax (2.0f, x1 - x0), h * 2.0f };
}

int NoteEditor::hitNote (juce::Point<float> p, bool& nearLeft, bool& nearRight) const
{
    nearLeft = nearRight = false;
    if (analysis == nullptr)
        return -1;
    for (int i = (int) notes.size() - 1; i >= 0; --i)
    {
        const auto b = noteBounds (notes[(size_t) i]);
        if (! b.contains (p))
            continue;
        const float edge = juce::jmin (7.0f, b.getWidth() * 0.25f);
        nearLeft = p.x - b.getX() < edge;
        nearRight = b.getRight() - p.x < edge;
        return i;
    }
    return -1;
}

// ============================================================ model sync
void NoteEditor::refreshFromEngine()
{
    seenModel = engine.getModelVersion();
    analysis = engine.getAnalysis();
    notes = engine.getNotes();
    spectrum = engine.getSpectrogram (specHop, specMin, specMax);
    if (analysis == nullptr)
        viewInitialised = false;
    else if (! viewInitialised && ! getLocalBounds().isEmpty())
    {
        zoomToFit();
        viewInitialised = true;
    }
    updateScrollBars();
    notifySelection();
    repaint();
}

void NoteEditor::commit()
{
    engine.setNotes (notes);
    seenModel = engine.getModelVersion();
    if (state.onNotesEdited) state.onNotesEdited();
    repaint();
}

void NoteEditor::notifySelection()
{
    if (state.onSelectionChanged) state.onSelectionChanged();
}

std::vector<int> NoteEditor::selectedIndices() const
{
    std::vector<int> r;
    for (int i = 0; i < (int) notes.size(); ++i)
        if (notes[(size_t) i].selected) r.push_back (i);
    return r;
}

void NoteEditor::beginGesture() { engine.pushUndo(); }

void NoteEditor::applyToSelection (const std::function<void (Note&)>& fn, bool pushUndo)
{
    if (selectedIndices().empty())
        return;
    if (pushUndo) engine.pushUndo();
    for (auto& n : notes)
        if (n.selected) fn (n);
    commit();
}

void NoteEditor::selectAll (bool select)
{
    for (auto& n : notes) n.selected = select;
    engine.setNotes (notes);
    seenModel = engine.getModelVersion();
    notifySelection();
    repaint();
}

void NoteEditor::undo() { if (engine.undo()) refreshFromEngine(); }
void NoteEditor::redo() { if (engine.redo()) refreshFromEngine(); }

void NoteEditor::correctPitchMacro (float centreAmount, float driftAmount)
{
    if (analysis == nullptr) return;
    if (selectedIndices().empty()) for (auto& n : notes) n.selected = true;
    engine.pushUndo();
    const Scale* sc = state.snap == SnapMode::scale ? &state.scale : nullptr;
    correctPitch (notes, centreAmount, driftAmount, sc);
    commit();
    notifySelection();
}

void NoteEditor::quantizeTimeMacro (double beatsPerGrid, float amount)
{
    if (analysis == nullptr) return;
    if (selectedIndices().empty()) for (auto& n : notes) n.selected = true;
    engine.pushUndo();
    const double grid = 60.0 / juce::jmax (1.0, state.bpm()) * beatsPerGrid * sr();
    quantizeTime (notes, grid, (double) engine.getTakeStart(), amount, *analysis);
    commit();
}

void NoteEditor::zoomToFit()
{
    const auto g = gridArea();
    if (g.isEmpty())
        return;
    if (analysis == nullptr || notes.empty())
    {
        viewStart = 0.0;
        samplesPerPx = 44100.0 * 8.0 / juce::jmax (1, g.getWidth());
        topMidi = 79.0f;
        pxPerSemi = 14.0f;
        updateScrollBars();
        repaint();
        return;
    }
    const double margin = 0.25 * sr();
    const double s0 = notes.front().outStart - margin, s1 = notes.back().outEnd + margin;
    viewStart = s0;
    samplesPerPx = juce::jmax (8.0, (s1 - s0) / g.getWidth());
    float lo = 200.0f, hi = -200.0f;
    for (auto& n : notes) { lo = juce::jmin (lo, n.center()); hi = juce::jmax (hi, n.center()); }
    lo -= 4.0f; hi += 4.0f;
    pxPerSemi = juce::jlimit (6.0f, 36.0f, (float) g.getHeight() / (hi - lo));
    topMidi = juce::jmin (maxMidi, hi + ((float) g.getHeight() / pxPerSemi - (hi - lo)) * 0.5f);
    updateScrollBars();
    repaint();
}

// ============================================================ painting
void NoteEditor::paint (juce::Graphics& g)
{
    g.fillAll (theme::bg);
    const auto area = gridArea();
    {
        juce::Graphics::ScopedSaveState ss (g);
        g.reduceClipRegion (area);
        paintGrid (g, area);
        if (showSpectrum) paintSpectrum (g, area);
        paintNotes (g, area);
        paintOverlay (g, area);
    }
    paintRuler (g);
    paintKeyboard (g);
    g.setColour (theme::panel);
    g.fillRect (getWidth() - scrollW, getHeight() - scrollW, scrollW, scrollW);
}

void NoteEditor::paintGrid (juce::Graphics& g, juce::Rectangle<int> area)
{
    const int lo = (int) std::floor (midiForY ((float) area.getBottom())), hi = (int) std::ceil (topMidi);
    for (int m = lo; m <= hi; ++m)
    {
        const float y0 = yForMidi ((float) m + 0.5f), y1 = yForMidi ((float) m - 0.5f);
        g.setColour (state.scale.contains (m) ? theme::rowIn : theme::rowOut);
        g.fillRect (juce::Rectangle<float> ((float) area.getX(), y0, (float) area.getWidth(), y1 - y0));
        g.setColour (((m % 12) + 12) % 12 == 0 ? theme::barLine : theme::gridLine.withAlpha (0.6f));
        g.drawHorizontalLine ((int) y1, (float) area.getX(), (float) area.getRight());
    }

    // beat / bar lines on the host timeline
    const double rate = sr();
    const double secPerBeat = 60.0 / juce::jmax (1.0, state.bpm());
    const int bpb = juce::jmax (1, state.beatsPerBar());
    const double takeStartSec = (double) engine.getTakeStart() / rate;
    const double t0 = takeStartSec + sampleForX ((float) area.getX()) / rate;
    const double t1 = takeStartSec + sampleForX ((float) area.getRight()) / rate;
    const double pxPerBeat = secPerBeat * rate / samplesPerPx;
    const int beatStep = pxPerBeat < 6.0 ? bpb * (pxPerBeat * bpb < 24.0 ? 4 : 1) : 1;
    for (long long b = (long long) std::floor (t0 / secPerBeat); b <= (long long) std::ceil (t1 / secPerBeat); ++b)
    {
        if (b % beatStep != 0) continue;
        const double s = (double) b * secPerBeat * rate - (double) engine.getTakeStart();
        const float x = xForSample (s);
        g.setColour (b % bpb == 0 ? theme::barLine : theme::gridLine);
        g.drawVerticalLine ((int) x, (float) area.getY(), (float) area.getBottom());
    }
}

void NoteEditor::paintSpectrum (juce::Graphics& g, juce::Rectangle<int> area)
{
    if (! spectrum.isValid())
        return;
    const double rps = (double) spectrum.getHeight() / (specMax - specMin);
    const int c0 = juce::jlimit (0, spectrum.getWidth() - 1, (int) std::floor (sampleForX ((float) area.getX()) / specHop));
    const int c1 = juce::jlimit (c0 + 1, spectrum.getWidth(), (int) std::ceil (sampleForX ((float) area.getRight()) / specHop) + 1);
    const float bottomMidi = midiForY ((float) area.getBottom());
    const int r0 = juce::jlimit (0, spectrum.getHeight() - 1, (int) std::floor ((specMax - topMidi) * rps));
    const int r1 = juce::jlimit (r0 + 1, spectrum.getHeight(), (int) std::ceil ((specMax - bottomMidi) * rps) + 1);

    const float dx0 = xForSample (c0 * specHop - specHop * 0.5), dx1 = xForSample (c1 * specHop - specHop * 0.5);
    const float dy0 = yForMidi ((float) (specMax - r0 / rps)), dy1 = yForMidi ((float) (specMax - r1 / rps));
    g.setOpacity (0.8f);
    g.setImageResamplingQuality (juce::Graphics::mediumResamplingQuality);
    g.drawImage (spectrum, (int) dx0, (int) dy0, (int) std::ceil (dx1 - dx0), (int) std::ceil (dy1 - dy0),
                 c0, r0, c1 - c0, r1 - r0);
    g.setOpacity (1.0f);
}

void NoteEditor::paintNotes (juce::Graphics& g, juce::Rectangle<int> area)
{
    if (analysis == nullptr)
        return;
    const auto& a = *analysis;
    const float visL = (float) area.getX() - 4.0f, visR = (float) area.getRight() + 4.0f;
    const bool showRaw = drag == Drag::param || drag == Drag::move || drag == Drag::pitchOnly;

    // voiced pitch that is not part of any note
    {
        std::vector<uint8_t> owned ((size_t) a.numFrames(), 0);
        for (auto& n : notes)
            for (int k = juce::jmax (0, n.startFrame); k < juce::jmin (a.numFrames(), n.endFrame); ++k) owned[(size_t) k] = 1;
        TimeMap tm;
        tm.build (notes, a.hop, a.numSamples);
        juce::Path p;
        bool open = false;
        for (int k = 0; k < a.numFrames(); ++k)
        {
            if (! a.voiced[(size_t) k] || owned[(size_t) k]) { open = false; continue; }
            const float x = xForSample (tm.toOutput ((double) k * a.hop));
            if (x < visL || x > visR) { open = false; continue; }
            const float y = yForMidi (a.midi[(size_t) k]);
            if (! open) p.startNewSubPath (x, y); else p.lineTo (x, y);
            open = true;
        }
        g.setColour (theme::textDim.withAlpha (0.45f));
        g.strokePath (p, juce::PathStrokeType (1.0f));
    }

    const juce::Font labelFont (juce::FontOptions (10.5f));
    for (int i = 0; i < (int) notes.size(); ++i)
    {
        const auto& n = notes[(size_t) i];
        const float x0 = xForSample (n.outStart), x1 = xForSample (n.outEnd);
        if (x1 < visL || x0 > visR)
            continue;
        const float cy = yForMidi (n.center());
        const float gainF = n.muted ? 0.25f : juce::jlimit (0.3f, 1.6f, std::sqrt (juce::Decibels::decibelsToGain (n.gainDb)));
        const int frames = n.endFrame - n.startFrame;
        const int step = juce::jmax (1, (int) ((float) frames / juce::jmax (1.0f, x1 - x0))); // ~1 point per pixel

        // amplitude blob
        juce::Path blob;
        bool first = true;
        std::vector<juce::Point<float>> bottom;
        for (int k = n.startFrame; k <= n.endFrame; k += step)
        {
            const int kk = juce::jmin (k, n.endFrame - 1);
            const float x = k >= n.endFrame ? x1 : xForSample (noteFrameToOut (n, k));
            float amp = std::sqrt (a.rms[(size_t) juce::jlimit (0, a.numFrames() - 1, kk)] / juce::jmax (1.0e-6f, a.maxRms));
            if (k == n.startFrame || k >= n.endFrame) amp *= 0.4f;
            const float h = juce::jlimit (1.2f, pxPerSemi * 1.1f, amp * pxPerSemi * 0.95f * gainF);
            if (first) blob.startNewSubPath (x, cy - h); else blob.lineTo (x, cy - h);
            first = false;
            bottom.push_back ({ x, cy + h });
            if (k >= n.endFrame) break;
            if (k + step > n.endFrame) k = n.endFrame - step;
        }
        for (auto it = bottom.rbegin(); it != bottom.rend(); ++it) blob.lineTo (*it);
        blob.closeSubPath();

        auto col = n.muted ? theme::blobMuted : (n.selected ? theme::blobSel : theme::blob);
        if (i == hoverNote && ! n.selected) col = col.brighter (0.25f);
        if (n.muted)
        {
            g.setColour (col.withAlpha (0.5f));
            g.strokePath (blob, juce::PathStrokeType (1.0f));
        }
        else
        {
            g.setGradientFill (juce::ColourGradient (col.brighter (0.2f).withAlpha (0.85f), 0, cy - pxPerSemi,
                                                     col.darker (0.3f).withAlpha (0.85f), 0, cy + pxPerSemi, false));
            g.fillPath (blob);
            g.setColour (col.darker (0.6f));
            g.strokePath (blob, juce::PathStrokeType (0.8f));
        }

        // raw (unedited) pitch while editing
        const auto slow = slowPitch (a, n);
        if (showRaw && n.selected)
        {
            juce::Path raw;
            bool open = false;
            for (int k = n.startFrame; k < n.endFrame; ++k)
            {
                if (! a.voicedAtFrame (k)) { open = false; continue; }
                const float x = xForSample (noteFrameToOut (n, k)), y = yForMidi (a.midi[(size_t) k]);
                if (! open) raw.startNewSubPath (x, y); else raw.lineTo (x, y);
                open = true;
            }
            g.setColour (theme::text.withAlpha (0.3f));
            g.strokePath (raw, juce::PathStrokeType (1.0f));
        }

        // edited pitch curve
        juce::Path curve;
        bool open = false;
        for (int k = n.startFrame; k < n.endFrame; ++k)
        {
            const float p = editedPitch (a, n, slow, k);
            if (p <= 0.0f) { open = false; continue; }
            const float x = xForSample (noteFrameToOut (n, k)), y = yForMidi (p);
            if (! open) curve.startNewSubPath (x, y); else curve.lineTo (x, y);
            open = true;
        }
        g.setColour (theme::pitchLine.withAlpha (n.muted ? 0.3f : 0.9f));
        g.strokePath (curve, juce::PathStrokeType (1.5f, juce::PathStrokeType::curved));

        // formant bar
        if (state.tool == Tool::formant || std::abs (n.formant) > 0.01f)
        {
            const float fy = cy - n.formant * pxPerSemi * 0.5f;
            g.setColour (theme::formant);
            g.drawLine (x0 + 3.0f, fy, juce::jmax (x0 + 4.0f, x1 - 3.0f), fy, 2.0f);
        }

        // boundary marker between touching notes
        if (i > 0 && std::abs (notes[(size_t) i - 1].outEnd - n.outStart) < a.hop)
        {
            g.setColour (theme::bg.withAlpha (0.9f));
            g.drawVerticalLine ((int) x0, cy - pxPerSemi * 0.9f, cy + pxPerSemi * 0.9f);
        }

        if (pxPerSemi >= 10.0f && x1 - x0 > 34.0f)
        {
            g.setFont (labelFont);
            g.setColour ((n.selected ? theme::blobSel : theme::text).withAlpha (0.85f));
            const auto label = juce::String (midiToName (n.center())) + " " + centsText (n.center());
            g.drawText (label, juce::Rectangle<float> (x0 + 2.0f, cy - pxPerSemi * 1.1f - 13.0f, 90.0f, 12.0f),
                        juce::Justification::left, false);
        }
    }
}

void NoteEditor::paintOverlay (juce::Graphics& g, juce::Rectangle<int> area)
{
    const auto cap = engine.getCaptureState();
    auto message = [&] (const juce::String& title, const juce::String& sub)
    {
        auto r = area.toFloat().withSizeKeepingCentre (juce::jmin (560.0f, (float) area.getWidth() - 40.0f), 96.0f);
        g.setColour (theme::panel.withAlpha (0.92f));
        g.fillRoundedRectangle (r, 8.0f);
        g.setColour (theme::border);
        g.drawRoundedRectangle (r, 8.0f, 1.0f);
        g.setColour (theme::text);
        g.setFont (juce::FontOptions (18.0f, juce::Font::bold));
        g.drawText (title, r.removeFromTop (52.0f).withTrimmedTop (14.0f), juce::Justification::centred);
        g.setColour (theme::textDim);
        g.setFont (juce::FontOptions (13.0f));
        g.drawFittedText (sub, r.reduced (16.0f, 4.0f).toNearestInt(), juce::Justification::centredTop, 2);
    };

    if (engine.isAnalysing())
    {
        message ("Detecting notes " + juce::String (juce::roundToInt (engine.getAnalysisProgress() * 100.0f)) + "%",
                 "Tracking pitch, finding notes and pitch marks...");
    }
    else if (cap == Engine::CaptureState::capturing)
    {
        message (juce::String::fromUTF8 ("\xe2\x97\x8f Recording ") + juce::String (engine.getCapturedSeconds(), 1) + " s",
                 "Let the song play through the vocal. Stop playback when you're done.");
    }
    else if (cap == Engine::CaptureState::armed)
    {
        message ("Ready to transfer", "Press play in FL Studio. NoteForge records this track's audio while it plays.");
    }
    else if (analysis == nullptr)
    {
        message ("Drop a vocal file here",
                 "Drag it from FL's browser or Explorer (or click Import...). Or put NoteForge first on the track, click TRANSFER and play the vocal.");
    }

    // imported file waiting to be lined up with the song
    if (analysis != nullptr && ! engine.isSynced() && ! engine.isAnalysing())
    {
        auto banner = area.toFloat().removeFromTop (34.0f).reduced (8.0f, 4.0f);
        g.setColour (theme::accent.withAlpha (0.9f));
        g.fillRoundedRectangle (banner, 6.0f);
        g.setColour (juce::Colours::black);
        g.setFont (juce::FontOptions (13.5f, juce::Font::bold));
        g.drawText ("Press play in FL Studio - NoteForge is listening to find where this file sits in your song",
                    banner.reduced (10.0f, 0.0f), juce::Justification::centredLeft);
    }

    // playhead (what is currently being heard)
    if (analysis != nullptr)
    {
        const auto ph = state.playheadSample();
        if (ph >= 0 && state.isPlaying())
        {
            const float x = xForSample ((double) (ph - engine.getTakeStart()));
            g.setColour (theme::playhead);
            g.drawLine (x, (float) area.getY(), x, (float) area.getBottom(), 1.5f);
        }
        {
            // marker line
            const float mx = xForSample (marker);
            const float dashes[] = { 4.0f, 4.0f };
            g.setColour (theme::accent.withAlpha (0.45f));
            g.drawDashedLine (juce::Line<float> (mx, (float) area.getY(), mx, (float) area.getBottom()), dashes, 2, 1.0f);
        }
        if (engine.isPreviewing())
        {
            const float x = xForSample (engine.getPreviewPosition());
            g.setColour (juce::Colours::limegreen.withAlpha (0.8f));
            g.drawLine (x, (float) area.getY(), x, (float) area.getBottom(), 1.0f);
        }
    }

    if (drag == Drag::rubber)
    {
        g.setColour (theme::accent.withAlpha (0.12f));
        g.fillRect (rubberBand);
        g.setColour (theme::accent.withAlpha (0.7f));
        g.drawRect (rubberBand, 1.0f);
    }

    if (state.tool == Tool::split && hoverNote >= 0)
    {
        const auto mp = getMouseXYRelative().toFloat();
        const auto b = noteBounds (notes[(size_t) hoverNote]);
        g.setColour (theme::text.withAlpha (0.8f));
        const float dashes[] = { 3.0f, 3.0f };
        g.drawDashedLine (juce::Line<float> (mp.x, b.getY() - 6.0f, mp.x, b.getBottom() + 6.0f), dashes, 2, 1.0f);
    }
}

void NoteEditor::paintRuler (juce::Graphics& g)
{
    auto r = juce::Rectangle<int> (keyW, 0, getWidth() - keyW - scrollW, rulerH);
    g.setColour (theme::panel);
    g.fillRect (r);
    g.setColour (theme::border);
    g.drawHorizontalLine (rulerH - 1, (float) r.getX(), (float) r.getRight());

    const double rate = sr();
    const double secPerBeat = 60.0 / juce::jmax (1.0, state.bpm());
    const int bpb = juce::jmax (1, state.beatsPerBar());
    const double secPerBar = secPerBeat * bpb;
    const double takeStartSec = (double) engine.getTakeStart() / rate;
    const double t0 = takeStartSec + sampleForX ((float) r.getX()) / rate;
    const double t1 = takeStartSec + sampleForX ((float) r.getRight()) / rate;
    const double pxPerBar = secPerBar * rate / samplesPerPx;
    const int barStep = pxPerBar >= 40.0 ? 1 : pxPerBar >= 12.0 ? 4 : 16;

    juce::Graphics::ScopedSaveState ss (g);
    g.reduceClipRegion (r);
    g.setFont (juce::FontOptions (11.0f));
    for (long long bar = (long long) std::floor (t0 / secPerBar); bar <= (long long) std::ceil (t1 / secPerBar); ++bar)
    {
        if (bar < 0 || bar % barStep != 0) continue;
        const float x = xForSample ((double) bar * secPerBar * rate - (double) engine.getTakeStart());
        g.setColour (theme::textDim);
        g.drawVerticalLine ((int) x, (float) rulerH - 9.0f, (float) rulerH);
        g.setColour (theme::text);
        g.drawText (juce::String (bar + 1), (int) x + 3, 2, 40, rulerH - 6, juce::Justification::left);
    }

    // take extent
    if (analysis != nullptr)
    {
        const float x0 = xForSample (0.0), x1 = xForSample ((double) analysis->numSamples);
        g.setColour (theme::accent.withAlpha (0.5f));
        g.fillRect (juce::Rectangle<float> (x0, (float) rulerH - 3.0f, x1 - x0, 3.0f));

        // playback marker tab (like FL's song position marker)
        const float mx = xForSample (marker);
        juce::Path tab;
        tab.addRoundedRectangle (mx - 6.0f, 2.0f, 12.0f, 9.0f, 2.0f);
        tab.addTriangle (mx - 6.0f, 10.0f, mx + 6.0f, 10.0f, mx, (float) rulerH - 2.0f);
        g.setColour (engine.isPreviewing() ? juce::Colours::limegreen : theme::accent);
        g.fillPath (tab);
        g.setColour (theme::bg);
        g.strokePath (tab, juce::PathStrokeType (1.0f));
    }
}

void NoteEditor::paintKeyboard (juce::Graphics& g)
{
    auto r = juce::Rectangle<int> (0, rulerH, keyW, getHeight() - rulerH - scrollW);
    g.setColour (theme::panel);
    g.fillRect (0, 0, keyW, rulerH);
    juce::Graphics::ScopedSaveState ss (g);
    g.reduceClipRegion (r);
    const int lo = (int) std::floor (midiForY ((float) r.getBottom())), hi = (int) std::ceil (topMidi);
    g.setFont (juce::FontOptions (10.0f));
    for (int m = lo; m <= hi; ++m)
    {
        const float y0 = yForMidi ((float) m + 0.5f), y1 = yForMidi ((float) m - 0.5f);
        const bool black = isBlackKey (m);
        const bool inScale = state.scale.contains (m);
        auto c = black ? juce::Colour (0xff2b2e35) : juce::Colour (0xffc9ced8);
        if (! inScale) c = c.interpolatedWith (theme::bg, 0.45f);
        if (m == soundingKey) c = theme::accent;
        g.setColour (c);
        g.fillRect (juce::Rectangle<float> (0.0f, y0, (float) keyW - 1.0f, y1 - y0));
        g.setColour (theme::bg.withAlpha (0.6f));
        g.drawHorizontalLine ((int) y1, 0.0f, (float) keyW);
        const bool isC = ((m % 12) + 12) % 12 == 0;
        if ((isC && pxPerSemi >= 8.0f) || pxPerSemi >= 12.0f)
        {
            // C gets its octave number; the other keys get their letter when there is room
            g.setColour (black ? theme::text.withAlpha (inScale ? 0.8f : 0.4f) : juce::Colour (0xff2b2e35));
            const auto name = juce::String (midiToName ((float) m));
            g.drawText (isC ? name : name.dropLastCharacters (m >= 0 && m < 12 ? 2 : 1),
                        juce::Rectangle<float> (2.0f, y0, (float) keyW - 6.0f, y1 - y0), juce::Justification::centredRight, false);
        }
        if (inScale && state.scale.mask != allNotesMask && pxPerSemi >= 7.0f)
        {
            g.setColour (theme::accent.withAlpha (0.8f));
            g.fillEllipse (4.0f, (y0 + y1) * 0.5f - 2.0f, 4.0f, 4.0f);
        }
    }
    g.setColour (theme::border);
    g.drawVerticalLine (keyW - 1, (float) r.getY(), (float) r.getBottom());
}

void NoteEditor::resized()
{
    hScroll.setBounds (keyW, getHeight() - scrollW, getWidth() - keyW - scrollW, scrollW);
    vScroll.setBounds (getWidth() - scrollW, rulerH, scrollW, getHeight() - rulerH - scrollW);
    if (analysis != nullptr && ! viewInitialised)
    {
        zoomToFit();
        viewInitialised = true;
    }
    updateScrollBars();
}

void NoteEditor::updateScrollBars()
{
    const auto g = gridArea();
    const double total = analysis ? (double) analysis->numSamples : 44100.0 * 8.0;
    const double visible = g.getWidth() * samplesPerPx;
    hScroll.setRangeLimits (juce::jmin (0.0, viewStart), juce::jmax (total, viewStart + visible), juce::dontSendNotification);
    hScroll.setCurrentRange (viewStart, visible, juce::dontSendNotification);
    const double visSemis = g.getHeight() / (double) pxPerSemi;
    vScroll.setRangeLimits (0.0, maxMidi - minMidi, juce::dontSendNotification);
    vScroll.setCurrentRange (maxMidi - topMidi, visSemis, juce::dontSendNotification);
}

void NoteEditor::scrollBarMoved (juce::ScrollBar* bar, double start)
{
    if (bar == &hScroll) viewStart = start;
    else topMidi = maxMidi - (float) start;
    repaint();
}

// ============================================================ interaction
void NoteEditor::audition (const Note& n)
{
    if (state.auditionOnClick && ! state.isPlaying())
        engine.startPreview (n.outStart, n.outEnd);
}

void NoteEditor::mouseMove (const juce::MouseEvent& e)
{
    bool l, r;
    const int h = hitNote (e.position, l, r);
    if (h != hoverNote || state.tool == Tool::split) { hoverNote = h; repaint(); }
    if (h >= 0 && (l || r) && (state.tool == Tool::main || state.tool == Tool::time))
        setMouseCursor (juce::MouseCursor::LeftRightResizeCursor);
    else if (h >= 0 && state.tool == Tool::split)
        setMouseCursor (juce::MouseCursor::IBeamCursor);
    else if (h >= 0 && (state.tool == Tool::modulation || state.tool == Tool::drift || state.tool == Tool::formant || state.tool == Tool::amplitude || state.tool == Tool::pitch))
        setMouseCursor (juce::MouseCursor::UpDownResizeCursor);
    else if (h >= 0)
        setMouseCursor (juce::MouseCursor::DraggingHandCursor);
    else
        setMouseCursor (juce::MouseCursor::NormalCursor);
}

void NoteEditor::mouseDown (const juce::MouseEvent& e)
{
    grabKeyboardFocus();
    dragStart = e.position;
    drag = Drag::none;
    gestureUndoPushed = false;
    timeEngaged = pitchEngaged = false;
    if (e.position.x < (float) keyW && e.position.y > (float) rulerH)
    {
        // click = hear the key; shift+click also turns that note on/off for snapping and correction
        const int m = (int) std::lround (midiForY (e.position.y));
        if (e.mods.isShiftDown())
        {
            // shift+click only toggles the note for tuning - no sound
            if (state.toggleNote) state.toggleNote (((m % 12) + 12) % 12);
            repaint();
            return;
        }
        if (state.playNote)
        {
            state.playNote (m, true);
            soundingKey = m;
            drag = Drag::keys;
        }
        repaint();
        return;
    }
    if (e.position.y < (float) rulerH && e.position.x >= (float) keyW && analysis != nullptr)
    {
        setMarker (sampleForX (e.position.x), true);
        drag = Drag::marker;
        return;
    }
    if (! gridArea().contains (e.getPosition()) || analysis == nullptr)
        return;

    if (e.mods.isMiddleButtonDown() || (e.mods.isAltDown() && e.mods.isCommandDown()))
    {
        drag = Drag::scrollView;
        dragInitial.clear();
        rubberBand = { (float) viewStart, topMidi, 0, 0 }; // stash view origin
        return;
    }

    bool nearL, nearR;
    const int hit = hitNote (e.position, nearL, nearR);

    if (state.tool == Tool::split)
    {
        if (hit < 0) return;
        engine.pushUndo();
        auto& n = notes[(size_t) hit];
        if (e.mods.isAltDown())
            mergeWithNext (notes, hit, *analysis);
        else
        {
            // convert the click position back to an original analysis frame within this note
            const double out = sampleForX (e.position.x);
            const double frac = (out - n.outStart) / juce::jmax (1.0, n.outEnd - n.outStart);
            const int frame = (int) std::lround (n.startFrame + frac * (n.endFrame - n.startFrame));
            for (auto& o : notes) o.selected = false;
            splitNote (notes, hit, frame, *analysis);
        }
        commit();
        notifySelection();
        return;
    }

    if (hit < 0)
    {
        if (! e.mods.isShiftDown() && ! e.mods.isCommandDown())
            for (auto& n : notes) n.selected = false;
        dragInitial = notes;
        drag = Drag::rubber;
        rubberBand = { e.position, e.position };
        notifySelection();
        repaint();
        return;
    }

    auto& hn = notes[(size_t) hit];
    if (! hn.selected)
    {
        if (! e.mods.isShiftDown() && ! e.mods.isCommandDown())
            for (auto& n : notes) n.selected = false;
        hn.selected = true;
    }
    else if (e.mods.isCommandDown())
        hn.selected = false;
    notifySelection();

    dragInitial = notes;
    dragNote = hit;
    switch (state.tool)
    {
        case Tool::main:
            drag = nearL ? Drag::stretchLeft : nearR ? Drag::stretchRight : Drag::move;
            break;
        case Tool::time:
            drag = nearL ? Drag::stretchLeft : nearR ? Drag::stretchRight : Drag::timeOnly;
            break;
        case Tool::pitch:
            drag = Drag::pitchOnly;
            break;
        default:
            drag = Drag::param;
            break;
    }
    if (hn.selected && (state.tool == Tool::main || state.tool == Tool::pitch))
        audition (hn);
    repaint();
}

void NoteEditor::mouseDrag (const juce::MouseEvent& e)
{
    if (drag == Drag::none || (analysis == nullptr && drag != Drag::keys))
        return;
    if (drag == Drag::keys)
    {
        const int m = (int) std::lround (midiForY (e.position.y));
        if (m != soundingKey && state.playNote)
        {
            state.playNote (soundingKey, false);
            state.playNote (m, true);
            soundingKey = m;
            repaint();
        }
        return;
    }
    if (drag == Drag::marker)
    {
        dragMarkerTo (e.position.x);
        return;
    }
    const float dx = e.position.x - dragStart.x, dy = e.position.y - dragStart.y;
    auto ensureUndo = [this]
    {
        if (! gestureUndoPushed) { engine.pushUndo(); gestureUndoPushed = true; }
    };

    switch (drag)
    {
        case Drag::scrollView:
            viewStart = rubberBand.getX() - dx * samplesPerPx;
            topMidi = juce::jlimit (minMidi + 6.0f, maxMidi, rubberBand.getY() + dy / pxPerSemi);
            updateScrollBars();
            repaint();
            return;

        case Drag::rubber:
        {
            rubberBand = juce::Rectangle<float> (dragStart, e.position);
            for (size_t i = 0; i < notes.size(); ++i)
                notes[i].selected = dragInitial[i].selected || noteBounds (notes[i]).intersects (rubberBand);
            notifySelection();
            repaint();
            return;
        }

        case Drag::move:
        case Drag::pitchOnly:
        case Drag::timeOnly:
        {
            if (! timeEngaged && drag != Drag::pitchOnly && std::abs (dx) > (drag == Drag::timeOnly ? 2.0f : 8.0f))
                timeEngaged = true;
            if (! pitchEngaged && drag != Drag::timeOnly && std::abs (dy) > juce::jmax (3.0f, pxPerSemi * 0.35f))
                pitchEngaged = true;
            if (! timeEngaged && ! pitchEngaged)
                return;
            ensureUndo();
            notes = dragInitial;
            if (timeEngaged)
                moveNotesInTime (notes, dragInitial, dx * samplesPerPx, *analysis);
            if (pitchEngaged)
            {
                const auto& anchor = dragInitial[(size_t) dragNote];
                float target = anchor.center() - dy / pxPerSemi;
                if (! e.mods.isAltDown())
                    target = state.snapPitch (target);
                const float delta = target - anchor.center();
                for (size_t i = 0; i < notes.size(); ++i)
                    if (notes[i].selected) notes[i].pitchOffset = dragInitial[i].pitchOffset + delta;
            }
            commit();
            return;
        }

        case Drag::stretchLeft:
        case Drag::stretchRight:
            ensureUndo();
            stretchNoteEdge (notes, dragInitial, dragNote, drag == Drag::stretchLeft, dx * samplesPerPx, *analysis);
            commit();
            return;

        case Drag::param:
        {
            if (std::abs (dy) < 2.0f && ! gestureUndoPushed)
                return;
            ensureUndo();
            const float v = -dy * (e.mods.isShiftDown() ? 0.2f : 1.0f);
            for (size_t i = 0; i < notes.size(); ++i)
            {
                if (! notes[i].selected) continue;
                const auto& o = dragInitial[i];
                auto& n = notes[i];
                switch (state.tool)
                {
                    case Tool::modulation: n.modScale = juce::jlimit (0.0f, 3.0f, o.modScale + v / 100.0f); break;
                    case Tool::drift:      n.driftScale = juce::jlimit (0.0f, 3.0f, o.driftScale + v / 100.0f); break;
                    case Tool::formant:    n.formant = juce::jlimit (-12.0f, 12.0f, o.formant + v / 15.0f); break;
                    case Tool::amplitude:  n.gainDb = juce::jlimit (-48.0f, 12.0f, o.gainDb + v / 5.0f); break;
                    default: break;
                }
            }
            commit();
            notifySelection();
            return;
        }

        case Drag::none:
            return;
    }
}

void NoteEditor::mouseUp (const juce::MouseEvent&)
{
    if (drag == Drag::keys && state.playNote && soundingKey >= 0)
        state.playNote (soundingKey, false);
    soundingKey = -1;
    if (drag == Drag::marker)
        setMarker (marker, true); // jump the running preview to where the marker was dropped
    if (drag == Drag::rubber)
    {
        engine.setNotes (notes);
        seenModel = engine.getModelVersion();
    }
    drag = Drag::none;
    notifySelection();
    repaint();
}

double NoteEditor::markerSongSeconds() const
{
    const double rate = engine.getTakeRate() > 0.0 ? engine.getTakeRate() : sr();
    return ((double) engine.getTakeStart() + marker) / rate;
}

void NoteEditor::dragMarkerTo (float x)
{
    // dragging past the left/right edge scrolls the view along with the marker (faster the further out)
    const auto g = gridArea();
    const float edge = 24.0f;
    float over = 0.0f;
    if (x < (float) g.getX() + edge) over = x - ((float) g.getX() + edge);
    else if (x > (float) g.getRight() - edge) over = x - ((float) g.getRight() - edge);
    if (over != 0.0f && analysis != nullptr)
    {
        const double step = (double) juce::jlimit (-60.0f, 60.0f, over * 0.5f) * samplesPerPx;
        const double maxStart = (double) analysis->numSamples - g.getWidth() * samplesPerPx * 0.5;
        viewStart = juce::jlimit (-g.getWidth() * samplesPerPx * 0.5, juce::jmax (0.0, maxStart), viewStart + step);
        updateScrollBars();
    }
    setMarker (sampleForX (juce::jlimit ((float) g.getX(), (float) g.getRight(), x)), false);
}

void NoteEditor::togglePreview()
{
    if (state.hostLinked())
    {
        // FL link: play/stop FL itself, starting from the marker (with the whole beat)
        engine.stopPreview();
        if (state.isPlaying()) { if (state.stopHost) state.stopHost(); }
        else if (state.playHostFrom) state.playHostFrom (markerSongSeconds());
        repaint();
        return;
    }
    if (engine.isPreviewing())
        engine.stopPreview();
    else if (analysis != nullptr && ! state.isPlaying())
    {
        if (marker >= analysis->numSamples - 1)
            marker = 0.0;
        engine.startPreview (marker, (double) analysis->numSamples);
    }
    repaint();
}

void NoteEditor::setMarker (double s, bool restartIfPlaying)
{
    const double len = analysis ? (double) analysis->numSamples : 0.0;
    marker = juce::jlimit (0.0, juce::jmax (0.0, len - 1.0), s);
    if (restartIfPlaying && engine.isPreviewing())
        engine.startPreview (marker, len);
    if (state.hostLinked() && state.moveHostPlayhead && analysis != nullptr)
        state.moveHostPlayhead (markerSongSeconds(), restartIfPlaying);
    repaint();
}

juce::String NoteEditor::getTooltip()
{
    const auto p = getMouseXYRelative();
    if (p.x < keyW && p.y > rulerH)
        return "Click a key to hear it (drag to play up and down). Shift+click: turn that note on/off for snapping and correction.";
    if (p.y < rulerH && p.x >= keyW)
        return state.hostLinked()
                   ? "Playback marker (linked to FL): click or drag to move FL's song position. Double-click or press P to play/stop FL from here; Home jumps to the start of the take."
                   : "Playback marker: click or drag to move it. Double-click or press P to play the edited vocal from here; Home jumps to the start. "
                     "Set up the FL link (button in the toolbar) to make this move FL's own playhead.";
    return {};
}

void NoteEditor::mouseDoubleClick (const juce::MouseEvent& e)
{
    if (e.position.y < (float) rulerH && e.position.x >= (float) keyW)
    {
        setMarker (sampleForX (e.position.x), false);
        if (! engine.isPreviewing()) togglePreview();
        else setMarker (marker, true);
        return;
    }
    bool l, r;
    const int hit = hitNote (e.position, l, r);
    if (hit < 0 || analysis == nullptr)
        return;
    engine.pushUndo();
    auto& n = notes[(size_t) hit];
    switch (state.tool)
    {
        case Tool::main:
        case Tool::pitch:
            // snap the pitch centre (to the nearest semitone when snapping is off)
            for (auto& o : notes)
                if (o.selected) o.pitchOffset += (state.snap == SnapMode::off ? std::round (o.center()) : state.snapPitch (o.center())) - o.center();
            break;
        case Tool::modulation: { const float v = n.modScale > 0.01f ? 0.0f : 1.0f; for (auto& o : notes) if (o.selected) o.modScale = v; break; }
        case Tool::drift:      { const float v = n.driftScale > 0.01f ? 0.0f : 1.0f; for (auto& o : notes) if (o.selected) o.driftScale = v; break; }
        case Tool::formant:    for (auto& o : notes) if (o.selected) o.formant = 0.0f; break;
        case Tool::amplitude:  for (auto& o : notes) if (o.selected) o.gainDb = 0.0f; break;
        case Tool::time:
        {
            auto init = notes;
            for (auto& o : init) o.selected = (&o - init.data()) == hit;
            moveNotesInTime (notes, init, n.inStart (analysis->hop) - n.outStart, *analysis);
            notes[(size_t) hit].selected = true;
            break;
        }
        case Tool::split: break;
    }
    commit();
    notifySelection();
}

void NoteEditor::mouseWheelMove (const juce::MouseEvent& e, const juce::MouseWheelDetails& w)
{
    const auto g = gridArea();
    // like FL Studio: the wheel over the timeline ruler zooms in/out around the mouse
    if ((e.mods.isCommandDown() && ! e.mods.isShiftDown()) || (e.position.y < (float) rulerH && e.position.x >= (float) keyW))
    {
        const double anchor = sampleForX (e.position.x);
        samplesPerPx = juce::jlimit (2.0, 4000.0, samplesPerPx * std::exp (-w.deltaY * 2.5));
        viewStart = anchor - (e.position.x - (float) g.getX()) * samplesPerPx;
    }
    else if (e.mods.isCommandDown() && e.mods.isShiftDown())
    {
        const float anchor = midiForY (e.position.y);
        pxPerSemi = juce::jlimit (5.0f, 48.0f, pxPerSemi * std::exp (w.deltaY * 2.5f));
        topMidi = anchor + (e.position.y - (float) g.getY()) / pxPerSemi;
    }
    else if (e.mods.isShiftDown())
        viewStart -= (w.deltaY + w.deltaX) * 400.0 * samplesPerPx;
    else
    {
        topMidi += w.deltaY * 14.0f;
        viewStart -= w.deltaX * 400.0 * samplesPerPx;
    }
    topMidi = juce::jlimit (minMidi + (float) g.getHeight() / pxPerSemi, maxMidi, topMidi);
    updateScrollBars();
    repaint();
}

void NoteEditor::mouseMagnify (const juce::MouseEvent& e, float scale)
{
    const double anchor = sampleForX (e.position.x);
    samplesPerPx = juce::jlimit (2.0, 4000.0, samplesPerPx / scale);
    viewStart = anchor - (e.position.x - (float) gridArea().getX()) * samplesPerPx;
    updateScrollBars();
    repaint();
}

void NoteEditor::transposeSelection (int dir, bool fine)
{
    if (selectedIndices().empty()) return;
    engine.pushUndo();
    for (auto& n : notes)
    {
        if (! n.selected) continue;
        if (fine) n.pitchOffset += 0.1f * (float) dir;
        else if (state.snap == SnapMode::scale) n.pitchOffset = (float) state.scale.step ((int) std::lround (n.center()), dir) - n.origCenter;
        else if (state.snap == SnapMode::chromatic) n.pitchOffset = std::round (n.center()) + (float) dir - n.origCenter;
        else n.pitchOffset += (float) dir;
    }
    commit();
    notifySelection();
}

bool NoteEditor::keyPressed (const juce::KeyPress& k)
{
    const auto mods = k.getModifiers();
    const int code = k.getKeyCode();
    if (mods.isCommandDown() && (code == 'Z' || code == 'z'))
    {
        if (mods.isShiftDown()) redo(); else undo();
        return true;
    }
    if (mods.isCommandDown() && (code == 'Y' || code == 'y')) { redo(); return true; }
    if (mods.isCommandDown() && (code == 'A' || code == 'a')) { selectAll (true); return true; }
    if (code == juce::KeyPress::escapeKey) { selectAll (false); return true; }
    if (code == juce::KeyPress::deleteKey || code == juce::KeyPress::backspaceKey)
    {
        const bool anyUnmuted = std::any_of (notes.begin(), notes.end(), [] (const Note& n) { return n.selected && ! n.muted; });
        applyToSelection ([anyUnmuted] (Note& n) { n.muted = anyUnmuted; });
        return true;
    }
    if (code == juce::KeyPress::upKey) { transposeSelection (1, mods.isAltDown()); return true; }
    if (code == juce::KeyPress::downKey) { transposeSelection (-1, mods.isAltDown()); return true; }
    if (code == juce::KeyPress::leftKey || code == juce::KeyPress::rightKey)
    {
        auto sel = selectedIndices();
        int idx = sel.empty() ? 0 : (code == juce::KeyPress::rightKey ? sel.back() + 1 : sel.front() - 1);
        idx = juce::jlimit (0, (int) notes.size() - 1, idx);
        if (notes.empty()) return true;
        for (auto& n : notes) n.selected = false;
        notes[(size_t) idx].selected = true;
        engine.setNotes (notes);
        seenModel = engine.getModelVersion();
        audition (notes[(size_t) idx]);
        notifySelection();
        repaint();
        return true;
    }
    if (code == '0') { applyToSelection ([] (Note& n) { resetNote (n); }); return true; }
    if (! mods.isAnyModifierKeyDown() && (code == 'P' || code == 'p')) { togglePreview(); return true; }
    if (code == juce::KeyPress::homeKey) { setMarker (0.0, true); viewStart = juce::jmin (viewStart, 0.0); updateScrollBars(); return true; }
    if (! mods.isAnyModifierKeyDown() && code >= '1' && code <= '8')
    {
        state.tool = (Tool) (code - '1');
        if (state.onToolChanged) state.onToolChanged();
        repaint();
        return true;
    }
    return false;
}

void NoteEditor::timerCallback()
{
    // keep scrolling while the marker is held past the edge of the view
    if (drag == Drag::marker && juce::ModifierKeys::currentModifiers.isLeftButtonDown())
    {
        const auto mp = getMouseXYRelative();
        const auto g = gridArea();
        if (mp.x < g.getX() + 24 || mp.x > g.getRight() - 24)
            dragMarkerTo ((float) mp.x);
    }
    if (drag == Drag::none && engine.getModelVersion() != seenModel)
        refreshFromEngine();

    const bool live = state.isPlaying() || engine.isPreviewing() || engine.isAnalysing()
                      || engine.getCaptureState() == Engine::CaptureState::capturing;
    // while FL is stopped, the marker follows FL's own song position when you move it in FL
    if (analysis != nullptr && ! state.isPlaying() && drag != Drag::marker)
    {
        const auto hp = state.playheadSample();
        if (hp >= 0 && hp != lastHostPosition)
        {
            if (lastHostPosition >= 0)
            {
                marker = juce::jlimit (0.0, juce::jmax (0.0, (double) analysis->numSamples - 1.0), (double) (hp - engine.getTakeStart()));
                repaint();
            }
            lastHostPosition = hp;
        }
    }
    const bool previewing = engine.isPreviewing();
    if (live && state.follow && (state.isPlaying() || previewing) && analysis != nullptr && drag == Drag::none)
    {
        const auto g = gridArea();
        const double pos = previewing && ! state.isPlaying() ? engine.getPreviewPosition()
                                                             : (double) (state.playheadSample() - engine.getTakeStart());
        const float x = xForSample (pos);
        if (pos >= 0.0 && (x > (float) g.getRight() - (float) g.getWidth() * 0.1f || x < (float) g.getX()))
        {
            viewStart = pos - g.getWidth() * 0.1 * samplesPerPx;
            updateScrollBars();
        }
    }
    if (live || wasLive)
        repaint();
    wasLive = live;
}
} // namespace nf
