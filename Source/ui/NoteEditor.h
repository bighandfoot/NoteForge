#pragma once
#include <juce_gui_basics/juce_gui_basics.h>
#include "Engine.h"
#include <functional>

namespace nf
{
enum class Tool { main, pitch, time, modulation, drift, formant, amplitude, split };
enum class SnapMode { scale, chromatic, off };

// UI state shared between the toolbar, the note editor and the inspector.
struct EditorState
{
    Tool tool = Tool::main;
    SnapMode snap = SnapMode::chromatic;
    Scale scale;
    bool auditionOnClick = true;
    bool follow = true;

    // Host info, supplied by the plugin (defaults keep the editor usable stand-alone).
    std::function<juce::int64()> playheadSample = [] { return (juce::int64) -1; };
    std::function<bool()> isPlaying = [] { return false; };
    std::function<double()> bpm = [] { return 120.0; };
    std::function<int()> beatsPerBar = [] { return 4; };

    std::function<void()> onSelectionChanged, onNotesEdited, onToolChanged;
    std::function<void (int pitchClass)> toggleNote;  // shift+click on the keyboard strip
    std::function<void (int midi, bool on)> playNote; // click on the keyboard strip: piano sound

    // Host transport link (FL bridge): when linked, the marker tab moves FL's song position and Play starts FL.
    std::function<bool()> hostLinked = [] { return false; };
    std::function<void (double songSeconds, bool force)> moveHostPlayhead;
    std::function<void (double songSeconds)> playHostFrom;
    std::function<void()> stopHost;

    float snapPitch (float midi) const
    {
        switch (snap)
        {
            case SnapMode::scale:     return scale.snap (midi);
            case SnapMode::chromatic: return std::round (midi);
            case SnapMode::off:       break;
        }
        return midi;
    }
};

// The Melodyne-style piano roll: note blobs, pitch curves, optional spectrogram backdrop.
class NoteEditor : public juce::Component, public juce::TooltipClient, private juce::Timer, private juce::ScrollBar::Listener
{
public:
    NoteEditor (Engine& engine, EditorState& state);
    ~NoteEditor() override;

    void paint (juce::Graphics&) override;
    void resized() override;
    void mouseMove (const juce::MouseEvent&) override;
    void mouseDown (const juce::MouseEvent&) override;
    void mouseDrag (const juce::MouseEvent&) override;
    void mouseUp (const juce::MouseEvent&) override;
    void mouseDoubleClick (const juce::MouseEvent&) override;
    void mouseWheelMove (const juce::MouseEvent&, const juce::MouseWheelDetails&) override;
    void mouseMagnify (const juce::MouseEvent&, float scale) override;
    bool keyPressed (const juce::KeyPress&) override;
    juce::String getTooltip() override;

    // ---- commands used by the toolbar / inspector ----
    const std::vector<Note>& getNotes() const noexcept { return notes; }
    std::vector<int> selectedIndices() const;
    void applyToSelection (const std::function<void (Note&)>& fn, bool pushUndo = true);
    void beginGesture();                 // push an undo snapshot before a continuous edit
    void selectAll (bool select);
    void undo();
    void redo();
    void correctPitchMacro (float centreAmount, float driftAmount);
    void quantizeTimeMacro (double beatsPerGrid, float amount);
    void zoomToFit();
    void setSpectrumVisible (bool shouldShow) { showSpectrum = shouldShow; repaint(); }
    bool isSpectrumVisible() const noexcept { return showSpectrum; }
    void refreshFromEngine();

    // ---- preview transport (the marker tab on the ruler, like FL's song position marker) ----
    void togglePreview();
    void setMarker (double takeSample, bool restartIfPlaying);
    double getMarker() const noexcept { return marker; }
    double getViewStart() const noexcept { return viewStart; }      // take samples at the left edge
    double getSamplesPerPixel() const noexcept { return samplesPerPx; }
    bool isPreviewing() const { return engine.isPreviewing(); }

private:
    enum class Drag { none, rubber, move, pitchOnly, timeOnly, stretchLeft, stretchRight, param, scrollView, marker, keys };

    // geometry
    juce::Rectangle<int> gridArea() const;
    float xForSample (double s) const;
    double sampleForX (float x) const;
    float yForMidi (float m) const;
    float midiForY (float y) const;
    double sr() const;

    int hitNote (juce::Point<float> p, bool& nearLeft, bool& nearRight) const;
    juce::Rectangle<float> noteBounds (const Note& n) const;
    double noteFrameToOut (const Note& n, double frame) const;

    void paintGrid (juce::Graphics&, juce::Rectangle<int> area);
    void paintSpectrum (juce::Graphics&, juce::Rectangle<int> area);
    void paintNotes (juce::Graphics&, juce::Rectangle<int> area);
    void paintRuler (juce::Graphics&);
    void paintKeyboard (juce::Graphics&);
    void paintOverlay (juce::Graphics&, juce::Rectangle<int> area);

    void commit();                       // push local notes to the engine
    void notifySelection();
    void updateScrollBars();
    void scrollBarMoved (juce::ScrollBar*, double newRangeStart) override;
    void timerCallback() override;
    void audition (const Note& n);
    void transposeSelection (int direction, bool fine);

    Engine& engine;
    EditorState& state;
    std::shared_ptr<const Analysis> analysis;
    std::vector<Note> notes;
    juce::uint32 seenModel = 0;

    // view
    double viewStart = 0.0;      // take-relative samples at the left edge of the grid
    double samplesPerPx = 200.0;
    float topMidi = 84.0f;       // pitch at the top edge
    float pxPerSemi = 14.0f;
    bool showSpectrum = true;
    bool viewInitialised = false;

    // interaction
    Drag drag = Drag::none;
    juce::Point<float> dragStart;
    std::vector<Note> dragInitial;
    int dragNote = -1;
    bool gestureUndoPushed = false;
    bool timeEngaged = false, pitchEngaged = false;
    juce::Rectangle<float> rubberBand;
    int hoverNote = -1;
    bool wasLive = false;
    double marker = 0.0;     // preview start, take-relative samples
    int soundingKey = -1;    // piano key held down on the keyboard strip
    juce::int64 lastHostPosition = -1; // to let the marker follow FL's playhead while stopped
    double markerSongSeconds() const;
    void dragMarkerTo (float x); // move the marker, auto-scrolling when dragged past the view edges

    juce::ScrollBar hScroll { false }, vScroll { true };
    juce::Image spectrum;
    double specHop = 512.0;
    float specMin = 24.0f, specMax = 108.0f;

    static constexpr int rulerH = 24, keyW = 56, scrollW = 12;
    static constexpr float minMidi = 24.0f, maxMidi = 108.0f;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (NoteEditor)
};
} // namespace nf
