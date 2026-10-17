#include "Engine.h"
#include "dsp/Align.h"
#include "dsp/PitchDetector.h"
#include "dsp/Renderer.h"

namespace nf
{
namespace
{
    juce::Colour heat (float v)
    {
        // dark navy -> violet -> orange -> pale yellow
        static const juce::Colour stops[] = { juce::Colour (0xff0b0d1a), juce::Colour (0xff35206b),
                                              juce::Colour (0xff9b2f7c), juce::Colour (0xfff26a3d),
                                              juce::Colour (0xfffde9a9) };
        v = juce::jlimit (0.0f, 1.0f, v) * 4.0f;
        const int i = juce::jmin (3, (int) v);
        return stops[i].interpolatedWith (stops[i + 1], v - (float) i);
    }

    template <typename T>
    juce::MemoryBlock toBlock (const std::vector<T>& v)
    {
        return juce::MemoryBlock (v.data(), v.size() * sizeof (T));
    }

    template <typename T>
    std::vector<T> fromBlock (const juce::var& v)
    {
        std::vector<T> out;
        if (auto* mb = v.getBinaryData())
        {
            out.resize (mb->getSize() / sizeof (T));
            std::memcpy (out.data(), mb->getData(), out.size() * sizeof (T));
        }
        return out;
    }

    bool sameEdit (const Note& a, const Note& b)
    {
        return a.startFrame == b.startFrame && a.endFrame == b.endFrame && a.pitchOffset == b.pitchOffset
               && a.modScale == b.modScale && a.driftScale == b.driftScale && a.formant == b.formant
               && a.gainDb == b.gainDb && a.muted == b.muted && a.outStart == b.outStart && a.outEnd == b.outEnd;
    }

    // 4-point Catmull-Rom interpolation (used when the host rate differs from the take's rate)
    float cubicAt (const std::vector<float>& x, double pos)
    {
        const int n = (int) x.size();
        const int i = (int) std::floor (pos);
        const float t = (float) (pos - i);
        auto at = [&] (int k) { return x[(size_t) juce::jlimit (0, n - 1, k)]; };
        const float y0 = at (i - 1), y1 = at (i), y2 = at (i + 1), y3 = at (i + 2);
        return y1 + 0.5f * t * (y2 - y0 + t * (2.0f * y0 - 5.0f * y1 + 4.0f * y2 - y3 + t * (3.0f * (y1 - y2) + y3 - y0)));
    }

    std::vector<float> resample (const std::vector<float>& in, double fromRate, double toRate)
    {
        const int outLen = juce::jmax (0, (int) std::floor ((double) in.size() * toRate / fromRate) - 4);
        juce::LagrangeInterpolator interp;
        // the interpolator delays its output by a fixed latency; skip it so timing stays sample-exact
        const int skip = (int) std::lround (juce::LagrangeInterpolator::getBaseLatency() * toRate / fromRate);
        std::vector<float> out ((size_t) (outLen + skip));
        std::vector<float> padded (in);
        padded.resize (in.size() + 64, 0.0f);
        interp.process (fromRate / toRate, padded.data(), out.data(), outLen + skip);
        out.erase (out.begin(), out.begin() + skip);
        return out;
    }
}

Engine::Engine() : juce::Thread ("NoteForge worker")
{
    for (auto& c : chunks)
        c.resize ((size_t) maxChunks);
    startThread (juce::Thread::Priority::normal);
}

Engine::~Engine()
{
    cancelAnalysis = true;
    stopThread (4000);
}

// ============================================================ capture
void Engine::armTransfer (double hostRate, int numChannels)
{
    if (analysing.load() || captureState.load() != CaptureState::idle)
        return;
    captureRate = hostRate;
    captureChannels = juce::jlimit (1, 2, numChannels);
    for (auto& c : chunks)
        for (auto& p : c) p.reset();
    for (int i = 0; i < 2; ++i)
        for (int ch = 0; ch < captureChannels.load(); ++ch)
            chunks[(size_t) ch][(size_t) i] = std::make_unique<float[]> ((size_t) chunkSize);
    chunksReady = 2;
    captureOrigin = -1;
    captureLength = 0;
    captureState = CaptureState::armed;
}

void Engine::disarmTransfer()
{
    auto s = captureState.load();
    if (s == CaptureState::armed)
    {
        captureState = CaptureState::idle;
        while (inCaptureBlock.load()) juce::Thread::yield();
        chunksReady = 0;
        for (auto& c : chunks)
            for (auto& p : c) p.reset();
    }
    else if (s == CaptureState::capturing)
    {
        captureState = CaptureState::finished;
        notify();
    }
}

double Engine::getCapturedSeconds() const noexcept
{
    return captureLength.load() / captureRate;
}

void Engine::captureBlock (juce::int64 pos, const float* const* in, int numIn, int n)
{
    inCaptureBlock = true; // set before reading the state so the worker can wait for us safely
    const auto s = captureState.load();
    if ((s == CaptureState::armed || s == CaptureState::capturing) && numIn > 0)
    {
        if (captureOrigin.load() < 0)
            captureOrigin = pos;
        const auto origin = captureOrigin.load();
        const int ready = chunksReady.load (std::memory_order_acquire);
        const int numCh = captureChannels.load();
        int len = captureLength.load();
        for (int i = 0; i < n; ++i)
        {
            const auto idx = pos + i - origin;
            if (idx < 0)
                continue;
            const auto chunk = idx >> chunkBits;
            if (chunk >= ready)
                break; // the worker has not caught up (should not happen); drop rather than block
            const auto off = (size_t) (idx & (chunkSize - 1));
            for (int ch = 0; ch < numCh; ++ch)
                chunks[(size_t) ch][(size_t) chunk][off] = in[juce::jmin (ch, numIn - 1)][i];
            len = juce::jmax (len, (int) idx + 1);
        }
        captureLength = len;
        if (s == CaptureState::armed)
        {
            CaptureState expected = CaptureState::armed;
            captureState.compare_exchange_strong (expected, CaptureState::capturing);
        }
    }
    inCaptureBlock = false;
}

void Engine::transportStopped()
{
    CaptureState expected = CaptureState::capturing;
    if (captureState.compare_exchange_strong (expected, CaptureState::finished))
        notify();
}

void Engine::ensureCaptureChunks()
{
    const auto s = captureState.load();
    if (s != CaptureState::armed && s != CaptureState::capturing)
        return;
    // keep at least half a chunk (~10 s) of headroom ahead of the write position
    while (chunksReady.load() < maxChunks
           && (juce::int64) chunksReady.load() * chunkSize < (juce::int64) captureLength.load() + chunkSize / 2)
    {
        const int next = chunksReady.load();
        for (int ch = 0; ch < captureChannels.load(); ++ch)
            chunks[(size_t) ch][(size_t) next] = std::make_unique<float[]> ((size_t) chunkSize);
        chunksReady.store (next + 1, std::memory_order_release);
    }
}

std::vector<std::vector<float>> Engine::collectCapture (double& rate, juce::int64& origin, int& length)
{
    while (inCaptureBlock.load())
        juce::Thread::yield();
    rate = captureRate;
    origin = juce::jmax ((juce::int64) 0, captureOrigin.load());
    length = captureLength.load();
    std::vector<std::vector<float>> out ((size_t) captureChannels.load());
    for (size_t ch = 0; ch < out.size(); ++ch)
    {
        out[ch].resize ((size_t) length);
        for (int i = 0; i < length; i += chunkSize)
        {
            const int count = juce::jmin (chunkSize, length - i);
            std::memcpy (out[ch].data() + i, chunks[ch][(size_t) (i >> chunkBits)].get(), (size_t) count * sizeof (float));
        }
    }
    chunksReady = 0;
    for (auto& c : chunks)
        for (auto& p : c) p.reset();
    return out;
}

// ============================================================ playback
void Engine::readPlayback (juce::int64 hostPos, double hostRate, float* const* out, int numOut, int n)
{
    if (! synced.load())
        return; // imported file not lined up with the song yet: let the original through
    const juce::SpinLock::ScopedTryLockType lock (audioLock);
    if (! lock.isLocked() || front.empty() || front[0].empty() || hostRate <= 0.0)
        return;
    const auto start = takeStart.load();
    const auto len = (juce::int64) front[0].size();
    const int numCh = (int) front.size();
    const double ratio = takeRate.load() / hostRate;

    if (std::abs (ratio - 1.0) < 1.0e-9)
    {
        for (int i = 0; i < n; ++i)
        {
            const auto idx = hostPos + i - start;
            if (idx < 0 || idx >= len) continue;
            for (int oc = 0; oc < numOut; ++oc)
                out[oc][i] = front[(size_t) juce::jmin (oc, numCh - 1)][(size_t) idx];
        }
        return;
    }
    for (int i = 0; i < n; ++i)
    {
        const double p = (double) (hostPos + i) * ratio - (double) start;
        if (p < 0.0 || p >= (double) (len - 1)) continue;
        for (int oc = 0; oc < numOut; ++oc)
            out[oc][i] = cubicAt (front[(size_t) juce::jmin (oc, numCh - 1)], p);
    }
}

void Engine::startPreview (double s, double e)
{
    previewEnd = e;
    previewPos = juce::jmax (0.0, s);
}

void Engine::stopPreview() { previewPos = -1.0; }

bool Engine::readPreview (double hostRate, float* const* out, int numOut, int n)
{
    double pos = previewPos.load();
    if (pos < 0.0)
        return false;
    const juce::SpinLock::ScopedTryLockType lock (audioLock);
    const double len = (lock.isLocked() && ! front.empty()) ? (double) front[0].size() : 0.0;
    const double end = juce::jmin (previewEnd.load(), len - 1.0);
    const double step = hostRate > 0.0 ? takeRate.load() / hostRate : 1.0;
    const int numCh = lock.isLocked() ? (int) front.size() : 0;
    for (int i = 0; i < n; ++i)
    {
        const double p = pos + i * step;
        for (int oc = 0; oc < numOut; ++oc)
        {
            float v = 0.0f;
            if (p < end && numCh > 0)
            {
                v = cubicAt (front[(size_t) juce::jmin (oc, numCh - 1)], p);
                const double fromEnd = end - p;
                if (fromEnd < 256.0) v *= (float) (fromEnd / 256.0); // short fade at the end
            }
            out[oc][i] = v;
        }
    }
    pos += n * step;
    previewPos = pos >= end ? -1.0 : pos;
    return true;
}

// ============================================================ model
bool Engine::hasTake() const
{
    const juce::ScopedLock sl (modelLock);
    return analysis != nullptr;
}

std::shared_ptr<const Analysis> Engine::getAnalysis() const
{
    const juce::ScopedLock sl (modelLock);
    return analysis;
}

double Engine::getTakeSampleRate() const
{
    const juce::ScopedLock sl (modelLock);
    return analysis ? analysis->sampleRate : 0.0;
}

std::vector<Note> Engine::getNotes() const
{
    const juce::ScopedLock sl (modelLock);
    return notes;
}

void Engine::setNotes (std::vector<Note> n)
{
    {
        const juce::ScopedLock sl (modelLock);
        notes = std::move (n);
    }
    ++editVersion;
    ++modelVersion;
    lastEditMs = juce::Time::getMillisecondCounter();
    notify();
}

void Engine::pushUndo()
{
    const juce::ScopedLock sl (modelLock);
    undoStack.push_back (notes);
    if (undoStack.size() > 200)
        undoStack.erase (undoStack.begin());
    redoStack.clear();
}

bool Engine::canUndo() const { const juce::ScopedLock sl (modelLock); return ! undoStack.empty(); }
bool Engine::canRedo() const { const juce::ScopedLock sl (modelLock); return ! redoStack.empty(); }

bool Engine::undo()
{
    std::vector<Note> n;
    {
        const juce::ScopedLock sl (modelLock);
        if (undoStack.empty())
            return false;
        redoStack.push_back (notes);
        n = std::move (undoStack.back());
        undoStack.pop_back();
    }
    setNotes (std::move (n));
    return true;
}

bool Engine::redo()
{
    std::vector<Note> n;
    {
        const juce::ScopedLock sl (modelLock);
        if (redoStack.empty())
            return false;
        undoStack.push_back (notes);
        n = std::move (redoStack.back());
        redoStack.pop_back();
    }
    setNotes (std::move (n));
    return true;
}

void Engine::clearTake()
{
    const juce::ScopedLock work (workLock);
    {
        const juce::ScopedLock sl (modelLock);
        analysis.reset();
        notes.clear();
        undoStack.clear();
        redoStack.clear();
    }
    {
        const juce::SpinLock::ScopedLockType al (audioLock);
        take.clear();
        front.clear();
        back.clear();
        takeLength = 0;
        takeChannels = 0;
        takeRate = 0.0;
    }
    {
        const juce::ScopedLock sl (specLock);
        spectrogram = {};
        specData = {};
    }
    renderedNotes.clear();
    fullRenderNeeded = true;
    syncWanted = false;
    synced = true;
    {
        const juce::ScopedLock il (importLock);
        importedName.clear();
        statusText.clear();
    }
    ++modelVersion;
    ++renderVersion;
}

juce::Image Engine::getSpectrogram (double& hop, float& mn, float& mx) const
{
    const juce::ScopedLock sl (specLock);
    hop = specHop;
    mn = specMin;
    mx = specMax;
    return spectrogram;
}

// ============================================================ worker
void Engine::run()
{
    while (! threadShouldExit())
    {
        ensureCaptureChunks();

        if (captureState.load() == CaptureState::finished)
            doAnalysis();
        if (importPending.load())
            doImport();
        if (syncReady.load())
            doSync();

        if (editVersion.load() != renderedEdit)
            doRender();
        else if (spectrumEdit != renderedEdit
                 && juce::Time::getMillisecondCounter() - lastEditMs.load() > 250)
            doSpectrogram();

        wait (30);
    }
}

std::vector<float> Engine::monoMix (const std::vector<std::vector<float>>& ch)
{
    if (ch.empty()) return {};
    if (ch.size() == 1) return ch[0];
    std::vector<float> m (ch[0].size());
    const float g = 1.0f / (float) ch.size();
    for (size_t i = 0; i < m.size(); ++i)
    {
        float v = 0.0f;
        for (auto& c : ch) v += c[i];
        m[i] = v * g;
    }
    return m;
}

void Engine::doAnalysis()
{
    analysing = true;
    progress = 0.0f;
    cancelAnalysis = false;

    double capRate;
    juce::int64 capOrigin;
    int capLen;
    auto cap = collectCapture (capRate, capOrigin, capLen);

    std::shared_ptr<const Analysis> old;
    std::vector<Note> oldNotes;
    {
        const juce::ScopedLock sl (modelLock);
        old = analysis;
        oldNotes = notes;
    }
    // an imported file that has not been lined up with the song yet has no timeline position to merge into
    const bool merge = old != nullptr && ! take.empty() && synced.load();
    const double rate = merge ? takeRate.load() : capRate;

    if (capLen > (int) (0.25 * capRate))
    {
        // bring the new audio to the take's sample rate
        juce::int64 c0 = capOrigin;
        if (std::abs (capRate - rate) > 0.5)
        {
            for (auto& ch : cap) ch = resample (ch, capRate, rate);
            c0 = (juce::int64) std::llround ((double) capOrigin * rate / capRate);
            capLen = cap.empty() ? 0 : (int) cap[0].size();
        }

        if (! merge)
        {
            auto mono = monoMix (cap);
            auto result = analyzeTake (mono.data(), (int) mono.size(), rate, &progress, &cancelAnalysis);
            if (! cancelAnalysis.load())
            {
                syncWanted = false;
                synced = true; // recorded takes know their timeline position
                installTake (std::move (cap), rate, c0, std::move (result));
            }
        }
        else
        {
            // ---- splice the new recording into the existing take, keeping edits elsewhere ----
            const int hop = old->hop;
            const juce::int64 t0 = takeStart.load();
            const int oldLen = takeLength.load();
            juce::int64 newStart = t0;
            if (c0 < t0)
                newStart = t0 - ((t0 - c0 + hop - 1) / hop) * hop; // keep frames aligned with the old analysis
            const juce::int64 newEnd = juce::jmax (t0 + oldLen, c0 + capLen);
            const int newLen = (int) (newEnd - newStart);
            const int numCh = (int) juce::jmax (take.size(), cap.size());

            std::vector<std::vector<float>> merged ((size_t) numCh, std::vector<float> ((size_t) newLen, 0.0f));
            for (int ch = 0; ch < numCh; ++ch)
            {
                const auto& o = take[(size_t) juce::jmin (ch, (int) take.size() - 1)];
                std::copy (o.begin(), o.end(), merged[(size_t) ch].begin() + (t0 - newStart));
                const auto& c = cap[(size_t) juce::jmin (ch, (int) cap.size() - 1)];
                std::copy (c.begin(), c.end(), merged[(size_t) ch].begin() + (c0 - newStart));
            }
            const auto mono = monoMix (merged);

            AnalysisResult r;
            auto& a = r.analysis;
            a.sampleRate = rate;
            a.hop = hop;
            a.numSamples = newLen;
            const int nf = newLen / hop + 1;
            a.rms.assign ((size_t) nf, 0.0f);
            a.rawHz.assign ((size_t) nf, 0.0f);
            a.aperiodicity.assign ((size_t) nf, 1.0f);

            PitchDetector det;
            det.prepare (rate, 55.0f, 1400.0f, offlineWindowMs); // same window as computeFrames
            const int frameOffset = (int) ((t0 - newStart) / hop);
            const int capRel = (int) (c0 - newStart);
            const int kA = (capRel - det.samplesNeeded() + det.windowSize() / 2) / hop - 2;
            const int kB = (capRel + capLen + det.windowSize() / 2) / hop + 2;
            const bool oldHasRaw = old->rawHz.size() == old->rms.size();

            std::vector<uint8_t> recompute ((size_t) nf, 1);
            for (int k = 0; k < nf; ++k)
            {
                const int ok = k - frameOffset;
                if (oldHasRaw && ok >= 0 && ok < (int) old->rms.size() && (k < kA || k > kB))
                {
                    a.rms[(size_t) k] = old->rms[(size_t) ok];
                    a.rawHz[(size_t) k] = old->rawHz[(size_t) ok];
                    a.aperiodicity[(size_t) k] = old->aperiodicity[(size_t) ok];
                    recompute[(size_t) k] = 0;
                }
            }
            for (int k = 0; k < nf && ! cancelAnalysis.load();)
            {
                if (! recompute[(size_t) k]) { ++k; continue; }
                int e = k;
                while (e < nf && recompute[(size_t) e]) ++e;
                computeFrames (a, mono.data(), k, e, &progress, &cancelAnalysis);
                k = e;
            }

            if (! cancelAnalysis.load())
            {
                r.notes = finalizeAnalysis (a, mono.data());

                // carry edits over from notes outside the re-recorded region
                const double shift = (double) (t0 - newStart);
                const double capStartIn = (double) capRel, capEndIn = (double) (capRel + capLen);
                for (auto& nn : r.notes)
                {
                    for (auto& on : oldNotes)
                    {
                        const int s = on.startFrame + frameOffset, e = on.endFrame + frameOffset;
                        const double inS = (double) s * hop, inE = (double) e * hop;
                        if (inE > capStartIn && inS < capEndIn)
                            continue; // this part was re-recorded
                        const int ov = juce::jmin (e, nn.endFrame) - juce::jmax (s, nn.startFrame);
                        const bool exact = s == nn.startFrame && e == nn.endFrame;
                        if (! exact && (ov < 0.7 * (e - s) || ov < 0.7 * (nn.endFrame - nn.startFrame)))
                            continue;
                        nn.pitchOffset = on.pitchOffset == 0.0f ? 0.0f : on.center() - nn.origCenter;
                        nn.modScale = on.modScale;
                        nn.driftScale = on.driftScale;
                        nn.formant = on.formant;
                        nn.gainDb = on.gainDb;
                        nn.muted = on.muted;
                        if (exact)
                        {
                            nn.outStart = on.outStart + shift;
                            nn.outEnd = on.outEnd + shift;
                        }
                        break;
                    }
                }
                installTake (std::move (merged), rate, newStart, std::move (r));
            }
        }
    }
    captureState = CaptureState::idle;
    analysing = false;
}

void Engine::installTake (std::vector<std::vector<float>> audio, double sr, juce::int64 start, AnalysisResult result, bool keepUndo)
{
    const juce::ScopedLock work (workLock);
    auto frontCopy = audio;
    auto backCopy = audio;
    {
        const juce::ScopedLock sl (modelLock);
        analysis = std::make_shared<const Analysis> (std::move (result.analysis));
        notes = std::move (result.notes);
        if (! keepUndo)
        {
            undoStack.clear();
            redoStack.clear();
        }
    }
    {
        const juce::SpinLock::ScopedLockType al (audioLock);
        take = std::move (audio);
        front = std::move (frontCopy);
        back = std::move (backCopy);
        takeStart = start;
        takeLength = take.empty() ? 0 : (int) take[0].size();
        takeChannels = (int) take.size();
        takeRate = sr;
    }
    {
        const juce::ScopedLock sl (specLock);
        specData = {};
    }
    renderedNotes.clear();
    fullRenderNeeded = true;
    staleInBack = {};
    ++editVersion;
    ++modelVersion;
    lastEditMs = 0;
    notify();
}

void Engine::doRender()
{
    const juce::ScopedLock work (workLock);
    std::shared_ptr<const Analysis> a;
    std::vector<Note> n;
    juce::uint32 ver;
    {
        const juce::ScopedLock sl (modelLock);
        a = analysis;
        n = notes;
        ver = editVersion.load();
    }
    if (a == nullptr || take.empty())
    {
        renderedEdit = ver;
        return;
    }

    const int len = takeLength.load();
    juce::Range<int> range;
    if (fullRenderNeeded || renderedNotes.size() != n.size())
        range = { 0, len };
    else
    {
        double lo = 1.0e18, hi = -1.0e18;
        for (size_t i = 0; i < n.size(); ++i)
        {
            if (sameEdit (renderedNotes[i], n[i]))
                continue;
            // a note's edit can affect the time map and boundary cross-fades up to its neighbours
            for (size_t j = (i > 0 ? i - 1 : 0); j <= juce::jmin (i + 1, n.size() - 1); ++j)
            {
                lo = juce::jmin (lo, renderedNotes[j].outStart, n[j].outStart, n[j].inStart (a->hop));
                hi = juce::jmax (hi, renderedNotes[j].outEnd, n[j].outEnd, n[j].inEnd (a->hop));
            }
        }
        if (hi < lo)
        {
            renderedNotes = n; // only the selection changed
            renderedEdit = ver;
            return;
        }
        const double pad = 0.25 * a->sampleRate; // covers boundary fades and the grain-chain re-lock
        range = { juce::jmax (0, (int) (lo - pad)), juce::jmin (len, (int) (hi + pad)) };
    }

    // bring `back` up to date with `front` where the previous render changed only `front`
    if (back.size() != front.size() || back.empty() || back[0].size() != front[0].size())
        back = front;
    else if (! staleInBack.isEmpty())
        for (size_t ch = 0; ch < front.size(); ++ch)
            std::copy (front[ch].begin() + staleInBack.getStart(), front[ch].begin() + staleInBack.getEnd(), back[ch].begin() + staleInBack.getStart());

    std::vector<const float*> inputs;
    for (auto& ch : take) inputs.push_back (ch.data());
    renderTake (inputs, *a, n, back, range.getStart(), range.getEnd());
    {
        const juce::SpinLock::ScopedLockType al (audioLock);
        std::swap (front, back);
    }
    staleInBack = range;
    lastRenderedRange = range;
    spectrumDirty = spectrumDirty.isEmpty() ? range : spectrumDirty.getUnionWith (range);
    renderedNotes = n;
    fullRenderNeeded = false;
    renderedEdit = ver;
    ++renderVersion;
}

void Engine::doSpectrogram()
{
    const juce::ScopedLock work (workLock);
    spectrumEdit = renderedEdit;
    auto a = getAnalysis();
    if (a == nullptr || front.empty())
        return;

    const auto mono = monoMix (front);
    const int hop = a->hop * 2;
    juce::Image img;
    SpectrogramData data;
    {
        const juce::ScopedLock sl (specLock);
        data = specData;
        img = spectrogram;
    }
    const bool partial = data.width == (int) mono.size() / hop + 1 && ! data.level.empty() && img.isValid()
                         && img.getWidth() == data.width && spectrumDirty.getLength() < (int) mono.size();
    int c0 = 0, c1 = 0;
    if (partial)
    {
        c0 = juce::jmax (0, spectrumDirty.getStart() / hop - 4);
        c1 = juce::jmin (data.width, spectrumDirty.getEnd() / hop + 5);
        updateSpectrogram (data, mono.data(), (int) mono.size(), a->sampleRate, c0, c1);
        img = img.createCopy();
    }
    else
    {
        data = computeSpectrogram (mono.data(), (int) mono.size(), a->sampleRate, hop);
        img = juce::Image (juce::Image::ARGB, data.width, data.height, false, juce::SoftwareImageType());
        c1 = data.width;
    }
    spectrumDirty = {};
    {
        juce::Image::BitmapData bd (img, juce::Image::BitmapData::writeOnly);
        for (int y = 0; y < data.height; ++y)
            for (int x = c0; x < c1; ++x)
                bd.setPixelColour (x, y, heat (data.level[(size_t) y * (size_t) data.width + (size_t) x]));
    }
    const juce::ScopedLock sl (specLock);
    spectrogram = img;
    specData = std::move (data);
    specHop = specData.hopSamples;
    specMin = specData.minMidi;
    specMax = specData.maxMidi;
    ++modelVersion;
}

// ============================================================ blocking helpers
void Engine::loadTakeBlocking (std::vector<std::vector<float>> audio, double sr, juce::int64 start)
{
    auto mono = monoMix (audio);
    auto result = analyzeTake (mono.data(), (int) mono.size(), sr);
    installTake (std::move (audio), sr, start, std::move (result));
}

void Engine::loadTakeBlocking (std::vector<float> mono, double sr, juce::int64 start)
{
    std::vector<std::vector<float>> a;
    a.push_back (std::move (mono));
    loadTakeBlocking (std::move (a), sr, start);
}

void Engine::renderBlocking()
{
    if (editVersion.load() != renderedEdit)
        doRender();
    doSpectrogram();
}

std::vector<std::vector<float>> Engine::getRenderedCopy() const
{
    const juce::SpinLock::ScopedLockType al (audioLock);
    return front;
}

bool Engine::waitUntilIdle (int timeoutMs)
{
    const auto end = juce::Time::getMillisecondCounter() + (juce::uint32) timeoutMs;
    while (juce::Time::getMillisecondCounter() < end)
    {
        const auto s = captureState.load();
        if (! analysing.load() && s != CaptureState::finished && editVersion.load() == renderedEdit)
            return true;
        juce::Thread::sleep (10);
    }
    return false;
}

// ============================================================ persistence
juce::ValueTree Engine::saveState() const
{
    juce::ValueTree t ("NoteForgeTake");
    const juce::ScopedLock work (workLock);
    auto a = getAnalysis();
    if (a == nullptr || take.empty())
        return t;

    t.setProperty ("version", 2, nullptr);
    t.setProperty ("sr", a->sampleRate, nullptr);
    t.setProperty ("start", (juce::int64) takeStart.load(), nullptr);
    t.setProperty ("hop", a->hop, nullptr);
    t.setProperty ("numSamples", a->numSamples, nullptr);
    t.setProperty ("channels", (int) take.size(), nullptr);
    t.setProperty ("synced", synced.load(), nullptr);
    {
        const juce::ScopedLock il (importLock);
        t.setProperty ("importedName", importedName, nullptr);
    }

    float peak = 0.0f;
    for (auto& ch : take)
        for (float v : ch)
            peak = juce::jmax (peak, std::abs (v));
    const float scale = peak > 0.999f ? 0.999f / peak : 1.0f;

    juce::MemoryBlock flac;
    {
        juce::FlacAudioFormat fmt;
        std::unique_ptr<juce::OutputStream> os = std::make_unique<juce::MemoryOutputStream> (flac, false);
        auto writer = fmt.createWriterFor (os, juce::AudioFormatWriterOptions{}
                                                   .withSampleRate (a->sampleRate)
                                                   .withNumChannels ((int) take.size())
                                                   .withBitsPerSample (24));
        if (writer != nullptr)
        {
            juce::AudioBuffer<float> buf ((int) take.size(), (int) take[0].size());
            for (int ch = 0; ch < (int) take.size(); ++ch)
                for (int i = 0; i < buf.getNumSamples(); ++i)
                    buf.setSample (ch, i, take[(size_t) ch][(size_t) i] * scale);
            writer->writeFromAudioSampleBuffer (buf, 0, buf.getNumSamples());
        }
    }
    if (flac.getSize() > 0)
    {
        t.setProperty ("flac", flac, nullptr);
        t.setProperty ("flacGain", 1.0f / scale, nullptr);
    }
    else
        for (int ch = 0; ch < (int) take.size(); ++ch)
            t.setProperty ("raw" + juce::String (ch), toBlock (take[(size_t) ch]), nullptr);

    t.setProperty ("rms", toBlock (a->rms), nullptr);
    t.setProperty ("rawHz", toBlock (a->rawHz), nullptr);
    t.setProperty ("aper", toBlock (a->aperiodicity), nullptr);

    juce::ValueTree nt ("Notes");
    for (auto& n : getNotes())
    {
        juce::ValueTree c ("N");
        c.setProperty ("sf", n.startFrame, nullptr);
        c.setProperty ("ef", n.endFrame, nullptr);
        c.setProperty ("oc", n.origCenter, nullptr);
        c.setProperty ("po", n.pitchOffset, nullptr);
        c.setProperty ("ms", n.modScale, nullptr);
        c.setProperty ("ds", n.driftScale, nullptr);
        c.setProperty ("fm", n.formant, nullptr);
        c.setProperty ("gd", n.gainDb, nullptr);
        c.setProperty ("mu", n.muted, nullptr);
        c.setProperty ("os", n.outStart, nullptr);
        c.setProperty ("oe", n.outEnd, nullptr);
        nt.appendChild (c, nullptr);
    }
    t.appendChild (nt, nullptr);
    return t;
}

void Engine::loadState (const juce::ValueTree& t)
{
    if (! t.hasType ("NoteForgeTake") || ! t.hasProperty ("sr"))
        return;

    const double sr = t["sr"];
    std::vector<std::vector<float>> audio;
    if (auto* mb = t["flac"].getBinaryData())
    {
        juce::FlacAudioFormat fmt;
        std::unique_ptr<juce::AudioFormatReader> reader (
            fmt.createReaderFor (new juce::MemoryInputStream (*mb, false), true));
        if (reader != nullptr)
        {
            juce::AudioBuffer<float> buf ((int) reader->numChannels, (int) reader->lengthInSamples);
            reader->read (&buf, 0, buf.getNumSamples(), 0, true, true);
            const float g = t.getProperty ("flacGain", 1.0f);
            audio.resize ((size_t) buf.getNumChannels());
            for (int ch = 0; ch < buf.getNumChannels(); ++ch)
            {
                audio[(size_t) ch].resize ((size_t) buf.getNumSamples());
                for (int i = 0; i < buf.getNumSamples(); ++i)
                    audio[(size_t) ch][(size_t) i] = buf.getSample (ch, i) * g;
            }
        }
    }
    else
    {
        for (int ch = 0; ch < 2; ++ch)
            if (auto v = fromBlock<float> (t["raw" + juce::String (ch)]); ! v.empty())
                audio.push_back (std::move (v));
        if (audio.empty())
            if (auto v = fromBlock<float> (t["raw"]); ! v.empty())
                audio.push_back (std::move (v));
    }
    if (audio.empty() || audio[0].empty())
        return;
    const auto mono = monoMix (audio);

    AnalysisResult r;
    auto& a = r.analysis;
    a.sampleRate = sr;
    a.hop = t["hop"];
    a.numSamples = (int) mono.size();
    a.rms = fromBlock<float> (t["rms"]);
    a.rawHz = fromBlock<float> (t["rawHz"]);
    a.aperiodicity = fromBlock<float> (t["aper"]);
    const auto legacyMidi = fromBlock<float> (t["midi"]);
    const auto legacyVoiced = fromBlock<uint8_t> (t["voiced"]);
    const size_t nf = (size_t) (a.numSamples / juce::jmax (1, a.hop) + 1);

    if (a.hop > 0 && a.rms.size() == nf && a.rawHz.size() == nf && a.aperiodicity.size() == nf)
        finalizeAnalysis (a, mono.data());
    else if (a.hop > 0 && a.rms.size() == nf && legacyMidi.size() == nf && legacyVoiced.size() == nf)
    {
        // version 1 projects stored the cleaned pitch only
        a.midi = legacyMidi;
        a.voiced = legacyVoiced;
        a.maxRms = *std::max_element (a.rms.begin(), a.rms.end());
        a.rawHz.resize (nf);
        a.aperiodicity.resize (nf);
        for (size_t k = 0; k < nf; ++k)
        {
            a.rawHz[k] = a.midi[k] > 0.0f ? midiToHz (a.midi[k]) : 0.0f;
            a.aperiodicity[k] = a.voiced[k] ? 0.0f : 1.0f;
        }
        computePitchMarks (a, mono.data());
    }
    else
        r = analyzeTake (mono.data(), (int) mono.size(), sr);

    const auto notesTree = t.getChildWithName ("Notes");
    if (notesTree.isValid())
    {
        r.notes.clear();
        for (const auto& c : notesTree)
        {
            Note n;
            n.startFrame = c["sf"];
            n.endFrame = c["ef"];
            n.origCenter = c["oc"];
            n.pitchOffset = c["po"];
            n.modScale = c["ms"];
            n.driftScale = c["ds"];
            n.formant = c["fm"];
            n.gainDb = c["gd"];
            n.muted = c["mu"];
            n.outStart = c["os"];
            n.outEnd = c["oe"];
            r.notes.push_back (n);
        }
    }
    installTake (std::move (audio), sr, (juce::int64) t["start"], std::move (r));
    {
        const juce::ScopedLock il (importLock);
        importedName = t.getProperty ("importedName", juce::String()).toString();
    }
    if (! (bool) t.getProperty ("synced", true))
    {
        synced = false;
        resetSyncBuffer();
        syncWanted = true;
    }
}

// ============================================================ import + automatic alignment
bool Engine::isSupportedAudioFile (const juce::File& f)
{
    return f.hasFileExtension ("wav;aif;aiff;flac;ogg;mp3");
}

void Engine::importAudioFile (const juce::File& file)
{
    {
        const juce::ScopedLock il (importLock);
        pendingImport = file;
        statusText = "Loading " + file.getFileName() + "...";
    }
    importPending = true;
    notify();
}

void Engine::resetSyncBuffer()
{
    // allocate the listening buffer on the calling (non-audio) thread before the audio thread may use it
    while (inSyncFeed.load()) juce::Thread::yield();
    syncReady = false;
    syncCount = 0;
    syncNext = -1;
    if (syncBuffer.empty())
        syncBuffer.assign ((size_t) (6.0 * 192000.0), 0.0f); // 6 s at up to 192 kHz
}

void Engine::doImport()
{
    juce::File file;
    {
        const juce::ScopedLock il (importLock);
        file = pendingImport;
    }
    importPending = false;
    analysing = true;
    progress = 0.0f;
    cancelAnalysis = false;

    juce::AudioFormatManager fm;
    fm.registerBasicFormats();
    std::unique_ptr<juce::AudioFormatReader> reader (fm.createReaderFor (file));
    if (reader == nullptr || reader->lengthInSamples < 1000)
    {
        const juce::ScopedLock il (importLock);
        statusText = "Couldn't read " + file.getFileName();
        analysing = false;
        return;
    }
    const int numCh = juce::jlimit (1, 2, (int) reader->numChannels);
    const int len = (int) juce::jmin ((juce::int64) std::numeric_limits<int>::max() / 2, reader->lengthInSamples);
    juce::AudioBuffer<float> buf ((int) reader->numChannels, len);
    reader->read (&buf, 0, len, 0, true, true);
    std::vector<std::vector<float>> audio ((size_t) numCh);
    for (int ch = 0; ch < numCh; ++ch)
        audio[(size_t) ch].assign (buf.getReadPointer (ch), buf.getReadPointer (ch) + len);
    const double rate = reader->sampleRate;

    auto mono = monoMix (audio);
    auto result = analyzeTake (mono.data(), (int) mono.size(), rate, &progress, &cancelAnalysis);
    if (! cancelAnalysis.load())
    {
        synced = false;
        installTake (std::move (audio), rate, 0, std::move (result));
        {
            const juce::ScopedLock il (importLock);
            importedName = file.getFileName();
            statusText = "Press play in FL Studio: NoteForge will find where " + importedName + " sits in your song.";
        }
        syncAttempts = 0;
        resetSyncBuffer();
        syncWanted = true;
    }
    analysing = false;
    ++modelVersion;
}

void Engine::feedSync (juce::int64 hostPos, double hostRate, const float* const* in, int numCh, int n)
{
    inSyncFeed = true; // set before checking the flags so resetSyncBuffer() can wait for us
    if (syncWanted.load() && ! syncReady.load() && numCh > 0 && ! syncBuffer.empty())
    {
        if (hostPos != syncNext.load())
        {
            // playback started or jumped: start listening again from here
            syncCount = 0;
            syncStart = hostPos;
            syncHostRate = hostRate;
        }
        const int cap = (int) juce::jmin ((double) syncBuffer.size(), 4.0 * hostRate); // listen to 4 s
        int count = syncCount.load();
        const float g = 1.0f / (float) numCh;
        for (int i = 0; i < n && count < cap; ++i)
        {
            float v = 0.0f;
            for (int ch = 0; ch < numCh; ++ch) v += in[ch][i];
            syncBuffer[(size_t) count++] = v * g;
        }
        syncCount = count;
        syncNext = hostPos + n;
        if (count >= cap)
        {
            syncReady = true;
            notify();
        }
    }
    inSyncFeed = false;
}

void Engine::doSync()
{
    while (inSyncFeed.load()) juce::Thread::yield();
    const int count = syncCount.load();
    const double hostRate = syncHostRate.load();
    const juce::int64 hostStart = syncStart.load();
    std::vector<float> chunk (syncBuffer.begin(), syncBuffer.begin() + count);

    double e = 0;
    for (float v : chunk) e += (double) v * v;
    if (count == 0 || std::sqrt (e / count) < 0.0015) // nothing audible yet (e.g. intro before the vocal)
    {
        syncCount = 0; syncNext = -1; syncReady = false;
        return;
    }

    const double rate = takeRate.load();
    if (std::abs (hostRate - rate) > 0.5)
        chunk = resample (chunk, hostRate, rate);
    std::vector<float> mono;
    {
        const juce::ScopedLock work (workLock);
        mono = monoMix (take);
    }
    const auto r = findChunkInTake (mono.data(), (int) mono.size(), chunk.data(), (int) chunk.size());
    if (r.confidence > 0.6f)
    {
        const auto hostStartTake = (juce::int64) std::llround ((double) hostStart * rate / hostRate);
        takeStart = hostStartTake - r.offset;
        synced = true;
        syncWanted = false;
        const juce::ScopedLock il (importLock);
        statusText = "Lined up " + importedName + juce::String::formatted (" with the song (match %.0f%%)", r.confidence * 100.0f);
    }
    else
    {
        ++syncAttempts;
        const juce::ScopedLock il (importLock);
        statusText = "Still listening for " + importedName + " in what FL plays"
                     + (syncAttempts >= 3 ? juce::String (" - is the clip on this mixer track? You can also use 'Place at playhead'.") : juce::String ("..."));
        syncCount = 0; syncNext = -1;
        syncReady = false;
    }
    ++modelVersion;
}

void Engine::placeAt (juce::int64 hostPos, double hostRate)
{
    const double rate = takeRate.load();
    if (rate <= 0.0 || hostRate <= 0.0)
        return;
    takeStart = (juce::int64) std::llround ((double) juce::jmax ((juce::int64) 0, hostPos) * rate / hostRate);
    syncWanted = false;
    synced = true;
    {
        const juce::ScopedLock il (importLock);
        statusText = "Placed " + importedName + " at the playhead";
    }
    ++modelVersion;
}

juce::String Engine::getStatusText() const
{
    const juce::ScopedLock il (importLock);
    return statusText;
}
} // namespace nf
