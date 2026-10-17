#pragma once
#include "PluginProcessor.h"
#include "ui/Inspector.h"
#include "ui/LiveView.h"
#include "ui/NoteEditor.h"
#include "ui/NotePicker.h"
#include "ui/Theme.h"
#include "ui/Widgets.h"
#include "ui/FlBridge.h"

class NoteForgeEditor : public juce::AudioProcessorEditor, public juce::FileDragAndDropTarget, private juce::Timer
{
public:
    explicit NoteForgeEditor (NoteForgeProcessor&);
    ~NoteForgeEditor() override;

    void paint (juce::Graphics&) override;
    void paintOverChildren (juce::Graphics&) override;
    void resized() override;

    // drag & drop audio files (from FL's browser or Explorer) to edit them without recording
    bool isInterestedInFileDrag (const juce::StringArray& files) override;
    void fileDragEnter (const juce::StringArray&, int, int) override { dragHover = true; repaint(); }
    void fileDragExit (const juce::StringArray&) override { dragHover = false; repaint(); }
    void filesDropped (const juce::StringArray& files, int, int) override;

private:
    void timerCallback() override;
    void updateMode();
    void updateToolButtons();
    void syncScale();
    void showCorrectPitch();
    void showQuantize();
    void onTransfer();
    void importFile (const juce::File& f);

    NoteForgeProcessor& proc;
    nf::theme::LookAndFeel lnf;
    nf::EditorState state;
    nf::NoteEditor noteEditor;
    nf::Inspector inspector;
    nf::LiveView liveView;
    nf::NotePicker notePicker;
    juce::TooltipWindow tooltips { this, 2000 };

    juce::TextButton transferButton { "TRANSFER" }, editModeButton { "EDIT" }, liveModeButton { "LIVE" };
    nf::TransportButton toStartButton { nf::TransportButton::Kind::toStart }, playButton { nf::TransportButton::Kind::playStop };
    juce::OwnedArray<juce::TextButton> toolButtons;
    juce::TextButton undoButton { "Undo" }, redoButton { "Redo" }, fitButton { "Fit" },
                     spectrumButton { "Spectrum" }, followButton { "Follow" },
                     correctButton { "Correct Pitch..." }, quantizeButton { "Quantize Time..." },
                     detectKeyButton { "Detect Key" }, clearButton { "Clear" };
    juce::ComboBox snapBox, keyBox, scaleBox;
    juce::Label snapLabel, keyLabel, status;

    juce::TextButton importButton { "Import..." }, placeButton { "Place at playhead" }, linkButton { "FL link" };
    nf::FlBridge bridge;
    void showLinkHelp();
    std::unique_ptr<juce::FileChooser> chooser;
    bool dragHover = false;
    juce::ScopedMessageBox messageBox;
    int blink = 0;
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (NoteForgeEditor)
};
