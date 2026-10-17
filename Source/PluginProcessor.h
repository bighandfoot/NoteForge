#pragma once
#include <juce_audio_processors/juce_audio_processors.h>
#include "Engine.h"
#include "dsp/LiveTuner.h"
#include "dsp/PianoSynth.h"

class NoteForgeProcessor : public juce::AudioProcessor, private juce::Timer
{
public:
    NoteForgeProcessor();
    ~NoteForgeProcessor() override;

    void prepareToPlay (double sampleRate, int samplesPerBlock) override;
    void releaseResources() override {}
    bool isBusesLayoutSupported (const BusesLayout&) const override;
    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override { return true; }

    const juce::String getName() const override { return "NoteForge"; }
    bool acceptsMidi() const override { return false; }
    bool producesMidi() const override { return false; }
    double getTailLengthSeconds() const override { return 0.0; }

    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram (int) override {}
    const juce::String getProgramName (int) override { return {}; }
    void changeProgramName (int, const juce::String&) override {}

    void getStateInformation (juce::MemoryBlock&) override;
    void setStateInformation (const void*, int) override;

    // ---- shared with the editor ----
    nf::Engine engine;
    nf::LiveTuner liveTuner;
    juce::AudioProcessorValueTreeState params;

    bool isLiveMode() const;
    bool isLowLatency() const { return engineParam->load() < 0.5f; }
    nf::Scale currentScale() const;
    unsigned getNoteMask() const;
    void setNoteMask (unsigned mask);          // message thread
    void setPreset (int root, int type);       // key + scale + matching note mask
    double getCurrentSampleRate() const noexcept { return currentRate.load(); }

    // Applies latency for the current mode (0 in Edit, PSOLA look-ahead in Live) and re-prepares the
    // live tuner when the voice range changes. Runs on a timer; public so tests can call it directly.
    void updateLatency();

    // Plays/stops a piano tone through the plugin output so a producer can hear a note (message thread).
    void playNote (int midi, bool on);

    static float lowestHzForRange (int range);
    static juce::StringArray rangeNames();

    std::atomic<juce::int64> heardPosition { -1 }; // host timeline sample currently audible
    std::atomic<bool> hostPlaying { false };
    std::atomic<double> hostBpm { 120.0 };
    std::atomic<int> hostBeatsPerBar { 4 };

    juce::ValueTree uiState { "UI" };

    // Live diagnostics (measured over ~50 ms of real time): current and peak share of real time spent in NoteForge,
    // and how many windows it used more than 70 % (only then could NoteForge itself cause dropouts).
    std::atomic<int> overBudgetCallbacks { 0 };
    std::atomic<float> peakLoad { 0.0f }, cpuLoad { 0.0f };
    void resetMeter() { overBudgetCallbacks = 0; peakLoad = 0.0f; }
    std::atomic<int> hostBlockSize { 0 };
    void reset() override { gapReset = true; }

private:
    static juce::AudioProcessorValueTreeState::ParameterLayout createLayout();
    void timerCallback() override { updateLatency(); }
    void processMain (juce::AudioBuffer<float>&);

    nf::PianoSynth piano;
    juce::AbstractFifo noteFifo { 64 };
    std::array<int, 64> noteEvents {}; // +midi+1 = on, -(midi+1) = off

    std::atomic<double> currentRate { 44100.0 };
    std::atomic<int> latency { 0 };
    int preparedRange = -1;
    juce::AudioBuffer<float> delayBuffer;
    int delayWrite = 0;
    std::vector<float> mono, rendered;
    bool wasPlaying = false, wasLive = false, wasLowLatency = true;
    juce::int64 trackedPos = 0, lastHostPos = 0; // our own timeline counter (see processMain)
    int lastBlockSize = 0;
    bool trackValid = false;
    std::atomic<bool> gapReset { false };
    double lastCallMs = 0.0, meterWindowStart = 0.0, meterBusyMs = 0.0;
    bool prepared = false;

    std::atomic<float>* modeParam = nullptr;
    std::atomic<float>* keyParam = nullptr;
    std::atomic<float>* scaleParam = nullptr;
    std::atomic<float>* rangeParam = nullptr;
    std::atomic<float>* retuneParam = nullptr;
    std::atomic<float>* amountParam = nullptr;
    std::atomic<float>* formantParam = nullptr;
    std::atomic<float>* transposeParam = nullptr;
    std::atomic<float>* mixParam = nullptr;
    std::atomic<float>* engineParam = nullptr;
    std::atomic<float>* bypassParam = nullptr;
    std::array<std::atomic<float>*, 12> noteParams {};

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (NoteForgeProcessor)
};
