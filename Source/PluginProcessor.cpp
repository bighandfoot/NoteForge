#include "PluginProcessor.h"
#include "ui/PluginEditor.h"

NoteForgeProcessor::NoteForgeProcessor()
    : AudioProcessor (BusesProperties()
                          .withInput ("Input", juce::AudioChannelSet::stereo(), true)
                          .withOutput ("Output", juce::AudioChannelSet::stereo(), true)),
      params (*this, nullptr, "Params", createLayout())
{
    modeParam = params.getRawParameterValue ("mode");
    keyParam = params.getRawParameterValue ("key");
    scaleParam = params.getRawParameterValue ("scale");
    rangeParam = params.getRawParameterValue ("range");
    retuneParam = params.getRawParameterValue ("retune");
    amountParam = params.getRawParameterValue ("amount");
    formantParam = params.getRawParameterValue ("formant");
    transposeParam = params.getRawParameterValue ("transpose");
    mixParam = params.getRawParameterValue ("mix");
    engineParam = params.getRawParameterValue ("liveEngine");
    bypassParam = params.getRawParameterValue ("liveBypass");
    for (int i = 0; i < 12; ++i)
        noteParams[(size_t) i] = params.getRawParameterValue ("note" + juce::String (i));
    startTimerHz (10);
}

NoteForgeProcessor::~NoteForgeProcessor() { stopTimer(); }

juce::StringArray NoteForgeProcessor::rangeNames()
{
    // "Auto" is appended (not inserted) so projects saved with High/Mid/Low keep their choice
    return { "High (soprano / female)", "Mid (alto / tenor)", "Low (baritone / bass)", "Auto (any voice, low to high)" };
}

float NoteForgeProcessor::lowestHzForRange (int range)
{
    static const float hz[] = { 140.0f, 90.0f, 60.0f, nf::LiveTuner::autoRangeHz };
    return hz[juce::jlimit (0, 3, range)];
}

juce::AudioProcessorValueTreeState::ParameterLayout NoteForgeProcessor::createLayout()
{
    using namespace juce;
    AudioProcessorValueTreeState::ParameterLayout l;
    StringArray keys, scales;
    for (auto& k : nf::keyNames()) keys.add (k);
    for (auto& s : nf::allScales()) scales.add (s.name);

    l.add (std::make_unique<AudioParameterChoice> (ParameterID { "mode", 1 }, "Mode", StringArray { "Edit", "Live" }, 0));
    l.add (std::make_unique<AudioParameterChoice> (ParameterID { "key", 1 }, "Key", keys, 0));
    l.add (std::make_unique<AudioParameterChoice> (ParameterID { "scale", 1 }, "Scale", scales, 0));
    for (int i = 0; i < 12; ++i)
        l.add (std::make_unique<AudioParameterBool> (ParameterID { "note" + String (i), 2 },
                                                     "Note " + String (nf::keyNames()[(size_t) i]), true));
    l.add (std::make_unique<AudioParameterChoice> (ParameterID { "range", 2 }, "Voice Range", rangeNames(), 3));
    l.add (std::make_unique<AudioParameterChoice> (ParameterID { "liveEngine", 3 }, "Live Engine",
                                                   StringArray { "Low latency (recording)", "High quality (mixing)" }, 0));
    l.add (std::make_unique<AudioParameterBool> (ParameterID { "liveBypass", 4 }, "Live Bypass", false));
    l.add (std::make_unique<AudioParameterFloat> (ParameterID { "retune", 1 }, "Retune Speed",
                                                  NormalisableRange<float> (0.0f, 400.0f, 0.1f, 0.4f), 20.0f,
                                                  AudioParameterFloatAttributes().withLabel ("ms")));
    l.add (std::make_unique<AudioParameterFloat> (ParameterID { "amount", 1 }, "Correction",
                                                  NormalisableRange<float> (0.0f, 100.0f, 0.1f), 100.0f,
                                                  AudioParameterFloatAttributes().withLabel ("%")));
    l.add (std::make_unique<AudioParameterFloat> (ParameterID { "formant", 1 }, "Formant",
                                                  NormalisableRange<float> (-12.0f, 12.0f, 0.01f), 0.0f,
                                                  AudioParameterFloatAttributes().withLabel ("st")));
    l.add (std::make_unique<AudioParameterFloat> (ParameterID { "transpose", 1 }, "Transpose",
                                                  NormalisableRange<float> (-12.0f, 12.0f, 1.0f), 0.0f,
                                                  AudioParameterFloatAttributes().withLabel ("st")));
    l.add (std::make_unique<AudioParameterFloat> (ParameterID { "mix", 1 }, "Mix",
                                                  NormalisableRange<float> (0.0f, 100.0f, 0.1f), 100.0f,
                                                  AudioParameterFloatAttributes().withLabel ("%")));
    return l;
}

bool NoteForgeProcessor::isBusesLayoutSupported (const BusesLayout& layouts) const
{
    const auto out = layouts.getMainOutputChannelSet();
    if (out != juce::AudioChannelSet::mono() && out != juce::AudioChannelSet::stereo())
        return false;
    return layouts.getMainInputChannelSet() == out;
}

bool NoteForgeProcessor::isLiveMode() const { return modeParam->load() > 0.5f; }

unsigned NoteForgeProcessor::getNoteMask() const
{
    unsigned m = 0;
    for (int i = 0; i < 12; ++i)
        if (noteParams[(size_t) i]->load() > 0.5f) m |= 1u << i;
    return m;
}

nf::Scale NoteForgeProcessor::currentScale() const
{
    return { (int) keyParam->load(), (int) scaleParam->load(), getNoteMask() };
}

void NoteForgeProcessor::setNoteMask (unsigned mask)
{
    if ((mask & nf::allNotesMask) == 0)
        mask = nf::allNotesMask;
    for (int i = 0; i < 12; ++i)
        if (auto* p = params.getParameter ("note" + juce::String (i)))
        {
            const float v = ((mask >> i) & 1u) ? 1.0f : 0.0f;
            if (p->getValue() != v)
                p->setValueNotifyingHost (v);
        }
}

void NoteForgeProcessor::setPreset (int root, int type)
{
    if (auto* p = params.getParameter ("key")) p->setValueNotifyingHost (p->convertTo0to1 ((float) root));
    if (auto* p = params.getParameter ("scale")) p->setValueNotifyingHost (p->convertTo0to1 ((float) type));
    setNoteMask (nf::presetMask (root, type));
}

void NoteForgeProcessor::updateLatency()
{
    if (! prepared)
        return;
    const int range = (int) rangeParam->load();
    if (range != preparedRange)
    {
        suspendProcessing (true);
        liveTuner.prepare (currentRate.load(), lowestHzForRange (range));
        preparedRange = range;
        suspendProcessing (false);
    }
    const int wanted = isLiveMode() ? liveTuner.latencySamples (isLowLatency()) : 0;
    if (wanted != latency.load())
    {
        latency = wanted;
        setLatencySamples (wanted);
    }
}

void NoteForgeProcessor::prepareToPlay (double sampleRate, int samplesPerBlock)
{
    currentRate = sampleRate;
    preparedRange = (int) rangeParam->load();
    liveTuner.prepare (sampleRate, lowestHzForRange (preparedRange));
    latency = isLiveMode() ? liveTuner.latencySamples (isLowLatency()) : 0;
    setLatencySamples (latency.load());

    const int maxBlock = juce::jmax (samplesPerBlock, 4096);
    // room for the largest live latency (lowest voice range)
    delayBuffer.setSize (juce::jmax (2, getTotalNumOutputChannels()), (int) (0.25 * sampleRate) + maxBlock + 1);
    delayBuffer.clear();
    delayWrite = 0;
    mono.assign ((size_t) maxBlock, 0.0f);
    rendered.assign ((size_t) maxBlock, 0.0f);
    piano.prepare (sampleRate);
    wasLive = isLiveMode();
    wasLowLatency = isLowLatency();
    prepared = true;
}

void NoteForgeProcessor::playNote (int midi, bool on)
{
    const auto scope = noteFifo.write (1);
    const int value = on ? midi + 1 : -(midi + 1);
    if (scope.blockSize1 > 0) noteEvents[(size_t) scope.startIndex1] = value;
    else if (scope.blockSize2 > 0) noteEvents[(size_t) scope.startIndex2] = value;
}

void NoteForgeProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer&)
{
    juce::ScopedNoDenormals noDenormals;
    const auto t0 = juce::Time::getHighResolutionTicks();
    const double nowMs = juce::Time::getMillisecondCounterHiRes();
    const double blockMs = 1000.0 * buffer.getNumSamples() / juce::jmax (1.0, currentRate.load());
    // the host stopped calling us for a while (e.g. FL smart disable): restart the live engine cleanly
    if (lastCallMs > 0.0 && nowMs - lastCallMs > juce::jmax (150.0, 8.0 * blockMs))
        gapReset = true;
    lastCallMs = nowMs;
    hostBlockSize = buffer.getNumSamples();

    processMain (buffer);

    // piano audition on top of whatever the plugin outputs
    const auto scope = noteFifo.read (noteFifo.getNumReady());
    auto handle = [this] (int start, int size)
    {
        for (int i = start; i < start + size; ++i)
        {
            const int e = noteEvents[(size_t) i];
            if (e > 0) piano.noteOn (e - 1); else piano.noteOff (-e - 1);
        }
    };
    handle (scope.startIndex1, scope.blockSize1);
    handle (scope.startIndex2, scope.blockSize2);
    if (piano.isActive())
        piano.render (buffer.getArrayOfWritePointers(), getTotalNumOutputChannels(), buffer.getNumSamples());

    // CPU meter: share of real (wall-clock) time spent in NoteForge over ~50 ms windows. Hosts like FL call
    // plugins with tiny slices of a buffer, so judging single calls against their own length is meaningless.
    const double usedMs = juce::Time::highResolutionTicksToSeconds (juce::Time::getHighResolutionTicks() - t0) * 1000.0;
    if (meterWindowStart <= 0.0)
        meterWindowStart = nowMs;
    meterBusyMs += usedMs;
    const double elapsed = nowMs + usedMs - meterWindowStart;
    if (elapsed >= 50.0)
    {
        const float load = (float) (meterBusyMs / elapsed);
        cpuLoad = load;
        if (load > peakLoad.load()) peakLoad = load;
        if (load > 0.7f) ++overBudgetCallbacks; // NoteForge alone used >70 % of real time in this window
        meterWindowStart = 0.0;
        meterBusyMs = 0.0;
    }
}

void NoteForgeProcessor::processMain (juce::AudioBuffer<float>& buffer)
{
    const int n = buffer.getNumSamples();
    const int numIn = getTotalNumInputChannels(), numOut = getTotalNumOutputChannels();
    for (int ch = numIn; ch < numOut; ++ch)
        buffer.clear (ch, 0, n);
    if ((size_t) n > mono.size())
        return; // host exceeded the announced block size: stay safe rather than allocate

    // ---- host transport ----
    const double rate = currentRate.load();
    const int lat = latency.load();
    juce::int64 pos = 0;
    bool playing = false;
    if (auto* ph = getPlayHead())
    {
        if (auto info = ph->getPosition())
        {
            playing = info->getIsPlaying();
            if (auto t = info->getTimeInSamples()) pos = *t;
            else if (auto s = info->getTimeInSeconds()) pos = (juce::int64) std::llround (*s * rate);
            if (auto b = info->getBpm()) hostBpm = *b;
            if (auto ts = info->getTimeSignature()) hostBeatsPerBar = juce::jmax (1, ts->numerator);
        }
    }
    // Some hosts split one audio buffer into several process calls but report the buffer's start position
    // for each of them (or report slightly rounded positions). Keep our own sample counter while playing and
    // only follow the host when it really jumps (playhead moved, loop restarted).
    if (playing)
    {
        const juce::int64 hostPos = pos;
        if (wasPlaying && trackValid)
        {
            const juce::int64 expected = trackedPos + lastBlockSize;
            if (hostPos == lastHostPos || std::abs (hostPos - expected) <= 2)
                pos = expected;   // repeated or rounded position: the audio is contiguous
            // else: genuine relocation, take the host's position as is
        }
        trackedPos = pos;
        trackValid = true;
        lastHostPos = hostPos;
        lastBlockSize = n;
    }
    else
        trackValid = false;
    hostPlaying = playing;
    heardPosition = pos - lat;

    // ---- transfer: record the untouched input at its timeline position ----
    if (playing && numIn > 0)
        engine.captureBlock (pos, buffer.getArrayOfReadPointers(), juce::jmin (2, numIn), n);
    // an imported file listens to what the host plays to find its place in the song
    if (playing && numIn > 0 && engine.wantsSync())
        engine.feedSync (pos, rate, buffer.getArrayOfReadPointers(), juce::jmin (2, numIn), n);
    if (! playing && wasPlaying)
        engine.transportStopped();
    if (playing && ! wasPlaying)
        engine.stopPreview();
    wasPlaying = playing;

    const bool live = isLiveMode();
    const bool lowLat = isLowLatency();
    if (gapReset.exchange (false) && live)
        liveTuner.resetAfterGap();
    if (live != wasLive || lowLat != wasLowLatency)
    {
        liveTuner.reset();
        wasLive = live;
        wasLowLatency = lowLat;
    }

    if (live)
    {
        nf::LiveTuner::Params p;
        p.scale = currentScale();
        p.retuneMs = retuneParam->load();
        p.amount = amountParam->load() * 0.01f;
        p.formant = formantParam->load();
        p.transpose = transposeParam->load();
        p.mix = mixParam->load() * 0.01f;
        p.lowLatency = lowLat;
        p.bypass = bypassParam->load() > 0.5f;
        // stereo in -> stereo out (a mono input is spread to every output)
        const int chans = juce::jlimit (1, 2, juce::jmin (numIn, numOut));
        liveTuner.process (buffer.getArrayOfReadPointers(), buffer.getArrayOfWritePointers(), chans, n, p);
        for (int ch = chans; ch < numOut; ++ch)
            buffer.copyFrom (ch, 0, buffer, 0, 0, n);
        return;
    }

    // ---- edit mode: dry signal (delayed only while a latency change is still settling) ----
    if (lat > 0)
    {
        const int dlen = delayBuffer.getNumSamples();
        for (int ch = 0; ch < numOut; ++ch)
        {
            auto* d = delayBuffer.getWritePointer (ch % delayBuffer.getNumChannels());
            auto* io = buffer.getWritePointer (ch);
            int w = delayWrite;
            for (int i = 0; i < n; ++i)
            {
                d[w] = io[i];
                int r = w - juce::jmin (lat, dlen - 1);
                if (r < 0) r += dlen;
                io[i] = d[r];
                if (++w == dlen) w = 0;
            }
        }
        delayWrite = (delayWrite + n) % dlen;
    }

    const auto cap = engine.getCaptureState();
    const bool capturing = cap == nf::Engine::CaptureState::armed || cap == nf::Engine::CaptureState::capturing;
    if (capturing || engine.getTakeLength() == 0)
        return;

    // replace whatever the take covers with the edited render (stereo if the take is stereo)
    if (playing)
        engine.readPlayback (pos - lat, rate, buffer.getArrayOfWritePointers(), numOut, n);
    else
        engine.readPreview (rate, buffer.getArrayOfWritePointers(), numOut, n);
}

juce::AudioProcessorEditor* NoteForgeProcessor::createEditor() { return new NoteForgeEditor (*this); }

void NoteForgeProcessor::getStateInformation (juce::MemoryBlock& dest)
{
    juce::ValueTree root ("NoteForgeState");
    root.setProperty ("version", 2, nullptr);
    root.appendChild (params.copyState(), nullptr);
    root.appendChild (engine.saveState(), nullptr);
    root.appendChild (uiState.createCopy(), nullptr);
    juce::MemoryOutputStream mos (dest, false);
    root.writeToStream (mos);
}

void NoteForgeProcessor::setStateInformation (const void* data, int size)
{
    const auto root = juce::ValueTree::readFromData (data, (size_t) size);
    if (! root.hasType ("NoteForgeState"))
        return;
    const auto p = root.getChildWithName (params.state.getType());
    if (p.isValid())
        params.replaceState (p);
    const auto take = root.getChildWithName ("NoteForgeTake");
    if (take.isValid() && take.hasProperty ("sr"))
        engine.loadState (take);
    else
        engine.clearTake();
    const auto ui = root.getChildWithName ("UI");
    if (ui.isValid())
        uiState.copyPropertiesFrom (ui, nullptr);
}

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter() { return new NoteForgeProcessor(); }
