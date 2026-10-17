#pragma once
#include <juce_audio_devices/juce_audio_devices.h>

namespace nf
{
// Moves FL Studio's song position / starts and stops playback from the plugin.
// VST3 has no way to control the host transport, so NoteForge sends SysEx to a virtual MIDI port
// (loopMIDI, port name containing "NoteForge") and a small FL controller script
// ("NoteForge Bridge", device_NoteForgeBridge.py) applies it with FL's transport API.
//
// Message: F0 7D 4E 46 <cmd> <ms0..ms4> F7   (7D = non-commercial ID, 'N' 'F'; ms = 35-bit, 7 bits per byte, LSB first)
//   cmd 1 = set song position, 2 = set position and play, 3 = stop
class FlBridge : private juce::Timer
{
public:
    FlBridge();
    ~FlBridge() override;

    bool isConnected() const noexcept { return output != nullptr; }
    juce::String portName() const { return connectedName; }

    void setPosition (double songSeconds, bool force = false); // force: always send (e.g. when a drag ends)
    void playFrom (double songSeconds);
    void stop();

    static juce::MidiMessage makeMessage (int command, double songSeconds);
    static bool decodeMessage (const juce::MidiMessage& m, int& command, double& songSeconds); // for tests

private:
    void timerCallback() override { connect(); }
    void connect();
    void send (int command, double seconds);

    std::unique_ptr<juce::MidiOutput> output;
    juce::String connectedName;
    double lastSentSeconds = -1.0;
    juce::uint32 lastSentMs = 0;
};
} // namespace nf
