#include "FlBridge.h"

namespace nf
{
FlBridge::FlBridge()
{
    connect();
    startTimer (2000); // pick up the port if loopMIDI is started (or the port created) later
}

FlBridge::~FlBridge() { stopTimer(); }

void FlBridge::connect()
{
    if (output != nullptr)
    {
        // drop the connection if the port disappeared
        for (auto& d : juce::MidiOutput::getAvailableDevices())
            if (d.name == connectedName) return;
        output.reset();
        connectedName.clear();
    }
    for (auto& d : juce::MidiOutput::getAvailableDevices())
    {
        if (! d.name.containsIgnoreCase ("NoteForge"))
            continue;
        if (auto out = juce::MidiOutput::openDevice (d.identifier))
        {
            output = std::move (out);
            connectedName = d.name;
            return;
        }
    }
}

juce::MidiMessage FlBridge::makeMessage (int command, double seconds)
{
    const auto ms = (juce::uint64) std::llround (juce::jmax (0.0, seconds) * 1000.0);
    juce::uint8 data[9] = { 0x7D, 0x4E, 0x46, (juce::uint8) command };
    for (int i = 0; i < 5; ++i)
        data[4 + i] = (juce::uint8) ((ms >> (7 * i)) & 0x7F);
    return juce::MidiMessage::createSysExMessage (data, (int) sizeof (data));
}

bool FlBridge::decodeMessage (const juce::MidiMessage& m, int& command, double& seconds)
{
    if (! m.isSysEx() || m.getSysExDataSize() < 9)
        return false;
    const auto* d = m.getSysExData();
    if (d[0] != 0x7D || d[1] != 0x4E || d[2] != 0x46)
        return false;
    command = d[3];
    juce::uint64 ms = 0;
    for (int i = 0; i < 5; ++i)
        ms |= (juce::uint64) (d[4 + i] & 0x7F) << (7 * i);
    seconds = (double) ms / 1000.0;
    return true;
}

void FlBridge::send (int command, double seconds)
{
    if (output == nullptr)
        connect();
    if (output != nullptr)
        output->sendMessageNow (makeMessage (command, seconds));
}

void FlBridge::setPosition (double seconds, bool force)
{
    // dragging the marker produces many moves: send at most ~40 per second, and only real changes
    const auto now = juce::Time::getMillisecondCounter();
    if (! force && (std::abs (seconds - lastSentSeconds) < 0.0005 || now - lastSentMs < 25))
        return;
    lastSentSeconds = seconds;
    lastSentMs = now;
    send (1, seconds);
}

void FlBridge::playFrom (double seconds)
{
    lastSentSeconds = seconds;
    send (2, seconds);
}

void FlBridge::stop() { send (3, 0.0); }
} // namespace nf
