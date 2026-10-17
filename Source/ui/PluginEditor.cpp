#include "PluginEditor.h"

using namespace nf;

namespace
{
    struct ToolInfo { Tool tool; const char* name; const char* tip; };
    const ToolInfo toolInfos[] = {
        { Tool::main,       "Main",    "Main tool (1): drag notes up/down in pitch and left/right in time, drag a note edge to stretch it, double-click to snap to the nearest note. Hold Alt for free pitch." },
        { Tool::pitch,      "Pitch",   "Pitch tool (2): drag notes up/down only. Double-click snaps the pitch centre. Alt = no snapping." },
        { Tool::time,       "Time",    "Time tool (3): move notes or drag their edges to stretch. Double-click restores the original timing." },
        { Tool::modulation, "Vibrato", "Pitch modulation tool (4): drag up/down to deepen or flatten vibrato. Double-click toggles flat / original." },
        { Tool::drift,      "Drift",   "Pitch drift tool (5): drag to straighten or exaggerate slow pitch drift and scoops. Double-click toggles straight / original." },
        { Tool::formant,    "Formant", "Formant tool (6): drag up/down to shift the vocal character without changing pitch. Double-click resets." },
        { Tool::amplitude,  "Amp",     "Amplitude tool (7): drag up/down to change a note's level. Double-click resets. Delete key mutes." },
        { Tool::split,      "Split",   "Note separation tool (8): click inside a note to split it. Alt+click merges it with the next note." },
    };

    // ------------------------------------------------------------- popups
    class CorrectPitchPanel : public juce::Component
    {
    public:
        explicit CorrectPitchPanel (std::function<void (float, float)> apply) : onApply (std::move (apply))
        {
            for (auto* s : { &centre, &drift })
            {
                s->setRange (0.0, 100.0, 1.0);
                s->setTextValueSuffix (" %");
                addAndMakeVisible (*s);
            }
            centre.setValue (100.0);
            drift.setValue (50.0);
            centreLabel.setText ("Pitch centre", juce::dontSendNotification);
            driftLabel.setText ("Pitch drift", juce::dontSendNotification);
            info.setText ("Applies to the selected notes (or all notes if none are selected). Uses the scale when Snap is set to Scale.",
                          juce::dontSendNotification);
            info.setFont (juce::FontOptions (12.0f));
            info.setColour (juce::Label::textColourId, theme::textDim);
            addAndMakeVisible (centreLabel);
            addAndMakeVisible (driftLabel);
            addAndMakeVisible (info);
            addAndMakeVisible (applyButton);
            applyButton.onClick = [this]
            {
                onApply ((float) centre.getValue() / 100.0f, (float) drift.getValue() / 100.0f);
                if (auto* box = findParentComponentOfClass<juce::CallOutBox>()) box->dismiss();
            };
            setSize (320, 170);
        }
        void resized() override
        {
            auto r = getLocalBounds().reduced (8);
            auto row = r.removeFromTop (28); centreLabel.setBounds (row.removeFromLeft (90)); centre.setBounds (row);
            r.removeFromTop (6);
            row = r.removeFromTop (28); driftLabel.setBounds (row.removeFromLeft (90)); drift.setBounds (row);
            r.removeFromTop (6);
            applyButton.setBounds (r.removeFromBottom (30));
            info.setBounds (r);
        }
    private:
        std::function<void (float, float)> onApply;
        juce::Slider centre { juce::Slider::LinearHorizontal, juce::Slider::TextBoxRight },
                     drift { juce::Slider::LinearHorizontal, juce::Slider::TextBoxRight };
        juce::Label centreLabel, driftLabel, info;
        juce::TextButton applyButton { "Correct" };
    };

    class QuantizePanel : public juce::Component
    {
    public:
        explicit QuantizePanel (std::function<void (double, float)> apply) : onApply (std::move (apply))
        {
            grid.addItemList ({ "1/4 note", "1/8 note", "1/16 note", "1/32 note" }, 1);
            grid.setSelectedId (3);
            amount.setRange (0.0, 100.0, 1.0);
            amount.setValue (100.0);
            amount.setTextValueSuffix (" %");
            gridLabel.setText ("Grid", juce::dontSendNotification);
            amountLabel.setText ("Strength", juce::dontSendNotification);
            for (juce::Component* c : { (juce::Component*) &grid, (juce::Component*) &amount, (juce::Component*) &gridLabel,
                                        (juce::Component*) &amountLabel, (juce::Component*) &applyButton })
                addAndMakeVisible (c);
            applyButton.onClick = [this]
            {
                const double beats[] = { 1.0, 0.5, 0.25, 0.125 };
                onApply (beats[juce::jlimit (0, 3, grid.getSelectedId() - 1)], (float) amount.getValue() / 100.0f);
                if (auto* box = findParentComponentOfClass<juce::CallOutBox>()) box->dismiss();
            };
            setSize (300, 110);
        }
        void resized() override
        {
            auto r = getLocalBounds().reduced (8);
            auto row = r.removeFromTop (26); gridLabel.setBounds (row.removeFromLeft (70)); grid.setBounds (row);
            r.removeFromTop (6);
            row = r.removeFromTop (26); amountLabel.setBounds (row.removeFromLeft (70)); amount.setBounds (row);
            applyButton.setBounds (r.removeFromBottom (28));
        }
    private:
        std::function<void (double, float)> onApply;
        juce::ComboBox grid;
        juce::Slider amount { juce::Slider::LinearHorizontal, juce::Slider::TextBoxRight };
        juce::Label gridLabel, amountLabel;
        juce::TextButton applyButton { "Quantize note starts" };
    };
}

// ===================================================================================
NoteForgeEditor::NoteForgeEditor (NoteForgeProcessor& p)
    : AudioProcessorEditor (p), proc (p), noteEditor (p.engine, state), inspector (noteEditor),
      liveView (p.params, p.liveTuner, [&p] { return p.currentScale(); },
                [&p] { return p.getLatencySamples() * 1000.0 / juce::jmax (1.0, p.getCurrentSampleRate()); })
{
    setLookAndFeel (&lnf);

    state.playheadSample = [this]
    {
        // host samples -> take samples (the take may have been recorded at another sample rate)
        const double tr = proc.engine.getTakeRate(), hr = proc.getCurrentSampleRate();
        const auto p = proc.heardPosition.load();
        return tr > 0.0 && hr > 0.0 && std::abs (tr - hr) > 0.5 ? (juce::int64) std::llround ((double) p * tr / hr) : p;
    };
    state.isPlaying = [this] { return proc.hostPlaying.load(); };
    state.bpm = [this] { return proc.hostBpm.load(); };
    state.beatsPerBar = [this] { return proc.hostBeatsPerBar.load(); };
    state.onSelectionChanged = [this] { inspector.update(); };
    state.onNotesEdited = [this] { inspector.update(); };
    state.onToolChanged = [this] { updateToolButtons(); };
    state.playNote = [this] (int midi, bool on) { proc.playNote (midi, on); };
    state.hostLinked = [this] { return bridge.isConnected(); };
    state.moveHostPlayhead = [this] (double s, bool force) { bridge.setPosition (s, force); };
    state.playHostFrom = [this] (double s) { bridge.playFrom (s); };
    state.stopHost = [this] { bridge.stop(); };
    // the note picker only chooses notes for tuning - it stays silent
    liveView.playNote = state.playNote;
    liveView.getDiagnostics = [this] (float& now, float& peak, int& over)
    {
        now = proc.cpuLoad.load();
        peak = proc.peakLoad.load();
        over = proc.overBudgetCallbacks.load();
    };
    liveView.resetDiagnostics = [this] { proc.resetMeter(); };
    state.snap = (SnapMode) (int) proc.uiState.getProperty ("snap", (int) SnapMode::chromatic);
    state.follow = proc.uiState.getProperty ("follow", true);
    noteEditor.setSpectrumVisible (proc.uiState.getProperty ("spectrum", true));
    syncScale();

    addAndMakeVisible (noteEditor);
    addAndMakeVisible (inspector);
    addChildComponent (liveView);

    // ---- row 1 ----
    transferButton.setTooltip ("Record (transfer) this track's audio while FL Studio plays, then stop playback to detect the notes. "
                               "Transfer again to add more parts of the song - edits elsewhere are kept.");
    transferButton.onClick = [this] { onTransfer(); };
    addAndMakeVisible (transferButton);

    importButton.setTooltip ("Open an audio file to edit (or just drag one onto NoteForge from FL's browser or Explorer). "
                             "Then press play in FL: NoteForge finds where the file sits in your song by itself.");
    importButton.onClick = [this]
    {
        chooser = std::make_unique<juce::FileChooser> ("Choose a vocal to edit", juce::File(), "*.wav;*.aif;*.aiff;*.flac;*.ogg;*.mp3");
        chooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
                              [this] (const juce::FileChooser& fc) { if (fc.getResult().existsAsFile()) importFile (fc.getResult()); });
    };
    addAndMakeVisible (importButton);
    placeButton.setTooltip ("Stop FL's playhead exactly where this file starts in your song, then click to place it there "
                            "(only needed if the automatic line-up can't find it).");
    placeButton.onClick = [this] { proc.engine.placeAt (proc.heardPosition.load(), proc.getCurrentSampleRate()); };
    addChildComponent (placeButton);

    toStartButton.setTooltip ("Move the playback marker back to the start of the take (Home)");
    toStartButton.onClick = [this] { noteEditor.setMarker (0.0, true); };
    playButton.setTooltip ("Play / stop from the marker tab on the ruler (P). With the FL link set up this starts FL itself "
                           "(with the beat); without it, it plays just the edited vocal while FL is stopped.");
    linkButton.setTooltip ("Link NoteForge to FL Studio's transport so the marker tab moves FL's playhead. Click for setup.");
    linkButton.onClick = [this] { showLinkHelp(); };
    addAndMakeVisible (linkButton);
    playButton.onClick = [this] { noteEditor.togglePreview(); };
    addAndMakeVisible (toStartButton);
    addAndMakeVisible (playButton);

    for (auto* b : { &editModeButton, &liveModeButton })
    {
        b->setRadioGroupId (200);
        b->setClickingTogglesState (true);
        addAndMakeVisible (*b);
    }
    editModeButton.setTooltip ("Edit mode: Melodyne-style note editing of the transferred take");
    liveModeButton.setTooltip ("Live mode: real-time automatic pitch correction (auto-tune)");
    auto setMode = [this] (int m)
    {
        if (auto* prm = proc.params.getParameter ("mode"))
            prm->setValueNotifyingHost (prm->convertTo0to1 ((float) m));
        updateMode();
    };
    editModeButton.onClick = [setMode] { setMode (0); };
    liveModeButton.onClick = [setMode] { setMode (1); };

    for (auto& ti : toolInfos)
    {
        auto* b = toolButtons.add (new juce::TextButton (ti.name));
        b->setTooltip (ti.tip);
        b->setRadioGroupId (100);
        b->setClickingTogglesState (true);
        const Tool tool = ti.tool;
        b->onClick = [this, tool] { state.tool = tool; noteEditor.repaint(); };
        addAndMakeVisible (b);
    }
    updateToolButtons();

    undoButton.onClick = [this] { noteEditor.undo(); };
    redoButton.onClick = [this] { noteEditor.redo(); };
    undoButton.setTooltip ("Undo (Ctrl+Z)");
    redoButton.setTooltip ("Redo (Ctrl+Y)");
    fitButton.setTooltip ("Zoom to fit the whole take");
    fitButton.onClick = [this] { noteEditor.zoomToFit(); };
    spectrumButton.setClickingTogglesState (true);
    spectrumButton.setToggleState (noteEditor.isSpectrumVisible(), juce::dontSendNotification);
    spectrumButton.setTooltip ("Show the frequency spectrum (harmonics) of the edited audio behind the notes");
    spectrumButton.onClick = [this]
    {
        noteEditor.setSpectrumVisible (spectrumButton.getToggleState());
        proc.uiState.setProperty ("spectrum", spectrumButton.getToggleState(), nullptr);
    };
    followButton.setClickingTogglesState (true);
    followButton.setToggleState (state.follow, juce::dontSendNotification);
    followButton.setTooltip ("Scroll with the playhead");
    followButton.onClick = [this]
    {
        state.follow = followButton.getToggleState();
        proc.uiState.setProperty ("follow", state.follow, nullptr);
    };
    for (auto* b : { &undoButton, &redoButton, &fitButton, &spectrumButton, &followButton })
        addAndMakeVisible (b);

    // ---- row 2 ----
    snapBox.addItemList ({ "Snap: Scale", "Snap: Chromatic", "Snap: Off" }, 1);
    snapBox.setSelectedId ((int) state.snap + 1, juce::dontSendNotification);
    snapBox.setTooltip ("What notes snap to when you drag them up/down (hold Alt to bypass)");
    snapBox.onChange = [this]
    {
        state.snap = (SnapMode) (snapBox.getSelectedId() - 1);
        proc.uiState.setProperty ("snap", (int) state.snap, nullptr);
    };
    addAndMakeVisible (snapBox);

    keyLabel.setText ("Key", juce::dontSendNotification);
    keyLabel.setJustificationType (juce::Justification::centredRight);
    keyLabel.setColour (juce::Label::textColourId, theme::textDim);
    addAndMakeVisible (keyLabel);
    {
        juce::StringArray keys, scales;
        for (auto& k : keyNames()) keys.add (k);
        for (auto& s : allScales()) scales.add (s.name);
        keyBox.addItemList (keys, 1);
        scaleBox.addItemList (scales, 1);
    }
    // the key/scale boxes are presets: choosing one lights the matching keys on the note picker
    auto applyPreset = [this]
    {
        proc.setPreset (keyBox.getSelectedId() - 1, scaleBox.getSelectedId() - 1);
        if (scaleBox.getSelectedId() > 1)
            snapBox.setSelectedId ((int) SnapMode::scale + 1, juce::sendNotification);
        syncScale();
        notePicker.repaint();
    };
    keyBox.onChange = applyPreset;
    scaleBox.onChange = applyPreset;
    keyBox.setTooltip ("Key (root note) of the scale preset");
    scaleBox.setTooltip ("Scale preset: sets which keys are lit on the note picker. You can then click keys to customise it.");
    addAndMakeVisible (keyBox);
    addAndMakeVisible (scaleBox);

    notePicker.getMask = [this] { return proc.getNoteMask(); };
    notePicker.getRoot = [this] { return proc.currentScale().root; };
    notePicker.setMask = [this] (unsigned m)
    {
        proc.setNoteMask (m);
        if ((m & allNotesMask) != allNotesMask)
            snapBox.setSelectedId ((int) SnapMode::scale + 1, juce::sendNotification);
        syncScale();
    };
    addAndMakeVisible (notePicker);
    state.toggleNote = [this] (int pc)
    {
        const unsigned m = proc.getNoteMask() ^ (1u << pc);
        notePicker.setMask (m);
        notePicker.repaint();
    };

    detectKeyButton.setTooltip ("Guess the key from the detected notes and switch Snap to Scale");
    detectKeyButton.onClick = [this]
    {
        std::vector<std::pair<float, float>> pw;
        for (auto& n : noteEditor.getNotes())
            pw.push_back ({ n.center(), (float) (n.outEnd - n.outStart) });
        if (pw.empty()) return;
        const auto k = estimateKey (pw);
        proc.setPreset (k.root, k.type);
        snapBox.setSelectedId ((int) SnapMode::scale + 1, juce::sendNotification);
        syncScale();
        notePicker.repaint();
    };
    correctButton.setTooltip ("Pull note centres onto the nearest note and straighten drift - Melodyne-style 'Correct Pitch' macro");
    correctButton.onClick = [this] { showCorrectPitch(); };
    quantizeButton.setTooltip ("Snap note starts to the project's beat grid");
    quantizeButton.onClick = [this] { showQuantize(); };
    clearButton.setTooltip ("Discard the transferred audio and all edits");
    clearButton.onClick = [this]
    {
        messageBox = juce::AlertWindow::showScopedAsync (
            juce::MessageBoxOptions().withIconType (juce::MessageBoxIconType::WarningIcon)
                .withTitle ("Clear take").withMessage ("Discard the transferred audio and all note edits?")
                .withButton ("Clear").withButton ("Cancel").withAssociatedComponent (this),
            [this] (int r) { if (r == 1) { proc.engine.clearTake(); noteEditor.refreshFromEngine(); } });
    };
    for (auto* b : { &detectKeyButton, &correctButton, &quantizeButton, &clearButton })
        addAndMakeVisible (b);

    status.setColour (juce::Label::textColourId, theme::textDim);
    status.setJustificationType (juce::Justification::centredRight);
    addAndMakeVisible (status);

    const int savedW = proc.uiState.getProperty ("w", 1320), savedH = proc.uiState.getProperty ("h", 800);
    setResizable (true, true);
    setResizeLimits (980, 600, 3200, 2000);
    setSize (savedW, savedH);
    state.scale = {};
    syncScale();
    updateMode();
    startTimerHz (15);
}

NoteForgeEditor::~NoteForgeEditor()
{
    stopTimer();
    setLookAndFeel (nullptr);
}

void NoteForgeEditor::updateToolButtons()
{
    for (int i = 0; i < toolButtons.size(); ++i)
        toolButtons[i]->setToggleState (toolInfos[i].tool == state.tool, juce::dontSendNotification);
}

void NoteForgeEditor::syncScale()
{
    const auto sc = proc.currentScale();
    if (sc.root != state.scale.root || sc.type != state.scale.type || sc.mask != state.scale.mask)
    {
        state.scale = sc;
        noteEditor.repaint();
        notePicker.repaint();
    }
    if (keyBox.getSelectedId() != sc.root + 1) keyBox.setSelectedId (sc.root + 1, juce::dontSendNotification);
    if (scaleBox.getSelectedId() != sc.type + 1) scaleBox.setSelectedId (sc.type + 1, juce::dontSendNotification);
}

void NoteForgeEditor::updateMode()
{
    const bool live = proc.isLiveMode();
    editModeButton.setToggleState (! live, juce::dontSendNotification);
    liveModeButton.setToggleState (live, juce::dontSendNotification);
    noteEditor.setVisible (! live);
    inspector.setVisible (! live);
    liveView.setVisible (live);
    for (auto* b : toolButtons) b->setVisible (! live);
    for (juce::Component* c : { (juce::Component*) &undoButton, (juce::Component*) &redoButton, (juce::Component*) &fitButton,
                                (juce::Component*) &spectrumButton, (juce::Component*) &followButton, (juce::Component*) &snapBox,
                                (juce::Component*) &transferButton, (juce::Component*) &toStartButton, (juce::Component*) &playButton,
                                (juce::Component*) &importButton, (juce::Component*) &linkButton,
                                (juce::Component*) &detectKeyButton, (juce::Component*) &correctButton,
                                (juce::Component*) &quantizeButton, (juce::Component*) &clearButton })
        c->setVisible (! live);
}

bool NoteForgeEditor::isInterestedInFileDrag (const juce::StringArray& files)
{
    for (auto& f : files)
        if (Engine::isSupportedAudioFile (juce::File (f))) return true;
    return false;
}

void NoteForgeEditor::filesDropped (const juce::StringArray& files, int, int)
{
    dragHover = false;
    repaint();
    for (auto& f : files)
        if (Engine::isSupportedAudioFile (juce::File (f)))
        {
            importFile (juce::File (f));
            return;
        }
}

void NoteForgeEditor::importFile (const juce::File& file)
{
    auto doImport = [this, file]
    {
        if (auto* prm = proc.params.getParameter ("mode")) prm->setValueNotifyingHost (0.0f);
        updateMode();
        proc.engine.importAudioFile (file);
        noteEditor.refreshFromEngine();
    };
    if (proc.engine.hasTake())
        messageBox = juce::AlertWindow::showScopedAsync (
            juce::MessageBoxOptions().withIconType (juce::MessageBoxIconType::QuestionIcon)
                .withTitle ("Open " + file.getFileName()).withMessage ("Replace the current take and its edits with this file?")
                .withButton ("Replace").withButton ("Cancel").withAssociatedComponent (this),
            [doImport] (int r) { if (r == 1) doImport(); });
    else
        doImport();
}

void NoteForgeEditor::paintOverChildren (juce::Graphics& g)
{
    if (! dragHover)
        return;
    auto r = getLocalBounds().toFloat().reduced (6.0f);
    g.setColour (theme::accent.withAlpha (0.12f));
    g.fillRoundedRectangle (r, 10.0f);
    g.setColour (theme::accent);
    const float dashes[] = { 10.0f, 6.0f };
    juce::Path border;
    border.addRoundedRectangle (r, 10.0f);
    juce::Path dashed;
    juce::PathStrokeType (3.0f).createDashedStroke (dashed, border, dashes, 2);
    g.fillPath (dashed);
    g.setFont (juce::FontOptions (26.0f, juce::Font::bold));
    g.drawText ("Drop the vocal here to edit it", r, juce::Justification::centred);
}

void NoteForgeEditor::showLinkHelp()
{
    const juce::String msg = bridge.isConnected()
        ? "Linked through the MIDI port \"" + bridge.portName() + "\".\n\n"
          "If FL's playhead doesn't move when you drag the marker tab, check step 3 below.\n\n"
        : juce::String ("Not linked yet.\n\n");
    messageBox = juce::AlertWindow::showScopedAsync (
        juce::MessageBoxOptions().withIconType (juce::MessageBoxIconType::InfoIcon)
            .withTitle ("FL Studio link")
            .withMessage (msg
                          + "Plugins can't move FL's playhead by themselves, so NoteForge talks to FL through a virtual MIDI cable:\n\n"
                            "1. Install loopMIDI (tobias-erichsen.de/software/loopmidi.html), open it, type NoteForge as the port name and click +.\n"
                            "2. The \"NoteForge Bridge\" script is already installed in your FL user data folder.\n"
                            "3. In FL: Options > MIDI settings > Input: select the NoteForge port, click Enable, and set Controller type to \"NoteForge Bridge (user)\".\n"
                            "4. Use Song mode in FL. Then the marker tab moves FL's playhead and the Play button starts FL from it.")
            .withButton ("OK").withAssociatedComponent (this),
        nullptr);
}

void NoteForgeEditor::onTransfer()
{
    auto& eng = proc.engine;
    const auto cap = eng.getCaptureState();
    if (cap == Engine::CaptureState::armed || cap == Engine::CaptureState::capturing)
    {
        eng.disarmTransfer();
        return;
    }
    // A new transfer is added to the existing take: only the part you play is (re)recorded,
    // edits everywhere else are kept. Use Clear to start over.
    if (auto* prm = proc.params.getParameter ("mode")) prm->setValueNotifyingHost (0.0f);
    updateMode();
    eng.armTransfer (proc.getCurrentSampleRate(), juce::jlimit (1, 2, proc.getTotalNumInputChannels()));
    noteEditor.refreshFromEngine();
}

void NoteForgeEditor::showCorrectPitch()
{
    auto panel = std::make_unique<CorrectPitchPanel> ([this] (float c, float d) { noteEditor.correctPitchMacro (c, d); });
    juce::CallOutBox::launchAsynchronously (std::move (panel), correctButton.getScreenBounds(), nullptr);
}

void NoteForgeEditor::showQuantize()
{
    auto panel = std::make_unique<QuantizePanel> ([this] (double beats, float amount) { noteEditor.quantizeTimeMacro (beats, amount); });
    juce::CallOutBox::launchAsynchronously (std::move (panel), quantizeButton.getScreenBounds(), nullptr);
}

void NoteForgeEditor::timerCallback()
{
    syncScale();
    updateMode();

    const auto cap = proc.engine.getCaptureState();
    ++blink;
    const bool rec = cap == Engine::CaptureState::capturing;
    const bool armed = cap == Engine::CaptureState::armed;
    transferButton.setButtonText (rec ? juce::String::fromUTF8 ("\xe2\x97\x8f REC ") + juce::String (proc.engine.getCapturedSeconds(), 1) + "s"
                                      : armed ? "ARMED" : "TRANSFER");
    transferButton.setColour (juce::TextButton::buttonColourId,
                              rec ? (blink % 8 < 4 ? theme::record : theme::record.darker (0.4f))
                                  : armed ? theme::record.darker (0.2f) : theme::panelHi);

    const bool linked = bridge.isConnected();
    const bool stopIcon = linked ? proc.hostPlaying.load() : noteEditor.isPreviewing();
    if (playButton.showStop != stopIcon)
    {
        playButton.showStop = stopIcon;
        playButton.repaint();
    }
    const juce::String linkText = linked ? juce::String::fromUTF8 ("FL link \xe2\x97\x8f") : juce::String ("FL link: off");
    if (linkButton.getButtonText() != linkText)
    {
        linkButton.setButtonText (linkText);
        linkButton.setColour (juce::TextButton::buttonColourId, linked ? juce::Colour (0xff2e7d4f) : theme::panelHi);
    }
    const bool showPlace = ! proc.isLiveMode() && proc.engine.hasTake() && ! proc.engine.isSynced();
    if (placeButton.isVisible() != showPlace) { placeButton.setVisible (showPlace); resized(); }
    undoButton.setEnabled (proc.engine.canUndo());
    redoButton.setEnabled (proc.engine.canRedo());

    juce::String s;
    if (proc.isLiveMode())
        s = "Live auto-tune";
    else if (proc.engine.hasTake())
    {
        const double tr = proc.engine.getTakeRate();
        s = "Take " + juce::String (proc.engine.getTakeLength() / tr, 1) + " s "
            + (proc.engine.getTakeChannels() > 1 ? "stereo" : "mono") + "  |  "
            + juce::String ((int) noteEditor.getNotes().size()) + " notes  |  0 ms delay";
    }
    if (! proc.isLiveMode())
    {
        const auto st = proc.engine.getStatusText();
        if (st.isNotEmpty() && (! proc.engine.isSynced() || proc.engine.isAnalysing() || s.isEmpty()))
            s = st;
    }
    status.setText (s, juce::dontSendNotification);
}

void NoteForgeEditor::paint (juce::Graphics& g)
{
    g.fillAll (theme::bg);
    g.setColour (theme::panel);
    g.fillRect (0, 0, getWidth(), 76);
    g.setColour (theme::border);
    g.drawHorizontalLine (76, 0.0f, (float) getWidth());
    g.setColour (theme::accent);
    g.setFont (juce::FontOptions (15.0f, juce::Font::bold));
    g.drawText ("NoteForge", getWidth() - 110, 8, 100, 24, juce::Justification::centredRight);
}

void NoteForgeEditor::resized()
{
    proc.uiState.setProperty ("w", getWidth(), nullptr);
    proc.uiState.setProperty ("h", getHeight(), nullptr);

    auto r = getLocalBounds();
    auto top = r.removeFromTop (76).reduced (8, 6);
    auto row1 = top.removeFromTop (30);
    top.removeFromTop (4);
    auto row2 = top;

    transferButton.setBounds (row1.removeFromLeft (100));
    row1.removeFromLeft (4);
    importButton.setBounds (row1.removeFromLeft (72));
    row1.removeFromLeft (8);
    editModeButton.setBounds (row1.removeFromLeft (56));
    row1.removeFromLeft (2);
    liveModeButton.setBounds (row1.removeFromLeft (56));
    row1.removeFromLeft (10);
    toStartButton.setBounds (row1.removeFromLeft (30));
    row1.removeFromLeft (2);
    playButton.setBounds (row1.removeFromLeft (30));
    row1.removeFromLeft (2);
    linkButton.setBounds (row1.removeFromLeft (74));
    row1.removeFromLeft (10);
    for (auto* b : toolButtons)
    {
        b->setBounds (row1.removeFromLeft (54));
        row1.removeFromLeft (3);
    }
    row1.removeFromRight (110); // logo
    for (auto* b : { &redoButton, &undoButton, &fitButton, &followButton, &spectrumButton })
    {
        b->setBounds (row1.removeFromRight (b == &spectrumButton ? 76 : 56));
        row1.removeFromRight (4);
    }

    snapBox.setBounds (row2.removeFromLeft (150));
    row2.removeFromLeft (10);
    keyLabel.setBounds (row2.removeFromLeft (32));
    row2.removeFromLeft (4);
    keyBox.setBounds (row2.removeFromLeft (60));
    row2.removeFromLeft (4);
    scaleBox.setBounds (row2.removeFromLeft (140));
    row2.removeFromLeft (6);
    notePicker.setBounds (row2.removeFromLeft (250));
    row2.removeFromLeft (8);
    detectKeyButton.setBounds (row2.removeFromLeft (90));
    row2.removeFromLeft (14);
    correctButton.setBounds (row2.removeFromLeft (120));
    row2.removeFromLeft (4);
    quantizeButton.setBounds (row2.removeFromLeft (124));
    row2.removeFromLeft (4);
    clearButton.setBounds (row2.removeFromLeft (60));
    row2.removeFromLeft (10);
    if (placeButton.isVisible())
    {
        placeButton.setBounds (row2.removeFromLeft (130));
        row2.removeFromLeft (8);
    }
    status.setBounds (row2);

    liveView.setBounds (r);
    inspector.setBounds (r.removeFromBottom (64));
    noteEditor.setBounds (r);
}
