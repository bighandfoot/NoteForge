#pragma once
#include <juce_audio_formats/juce_audio_formats.h>
#include <juce_data_structures/juce_data_structures.h>
#include <juce_graphics/juce_graphics.h>
#include "dsp/Analysis.h"
#include "dsp/Spectrogram.h"
#include <array>
#include <atomic>
#include <memory>

namespace nf
{
// Owns the transferred take (mono or stereo), its analysis, the user's note edits and the
// rendered result. Heavy work (analysis, rendering, spectrogram) runs on an internal worker thread.
//
// Timeline positions handed to the engine by the host are in host samples at the host rate;
// everything stored (take start, note times) is in samples at the take's own rate.
class Engine : private juce::Thread
{
public:
    Engine();
    ~Engine() override;

    // ---------------- transfer (capture) ----------------
    enum class CaptureState { idle, armed, capturing, finished };
    void armTransfer (double hostRate, int numChannels); // message thread (allocates)
    void disarmTransfer();                                // message thread
    CaptureState getCaptureState() const noexcept { return captureState.load(); }
    double getCapturedSeconds() const noexcept;
    void captureBlock (juce::int64 hostPos, const float* const* channels, int numChannels, int numSamples); // audio thread
    void transportStopped();                                                                               // audio thread

    // ---------------- import (drag & drop) ----------------
    // Loads an audio file as the take (replacing any current one). Its place in the song is found
    // automatically the next time the host plays it ("sync"), or set with placeAt().
    void importAudioFile (const juce::File& file);                   // message thread
    bool isSynced() const noexcept { return synced.load(); }
    bool wantsSync() const noexcept { return syncWanted.load(); }
    void feedSync (juce::int64 hostPos, double hostRate, const float* const* in, int numCh, int n); // audio thread
    void placeAt (juce::int64 hostPos, double hostRate);              // message thread: manual placement
    juce::String getStatusText() const;                               // import / sync progress for the UI
    static bool isSupportedAudioFile (const juce::File& f);

    // ---------------- playback (audio thread) ----------------
    // Overwrites the samples of [hostPos, hostPos + n) that the take covers; leaves the rest untouched.
    void readPlayback (juce::int64 hostPos, double hostRate, float* const* out, int numOut, int numSamples);
    void startPreview (double takeStartSample, double takeEndSample); // message thread
    void stopPreview();
    bool isPreviewing() const noexcept { return previewPos.load() >= 0.0; }
    bool readPreview (double hostRate, float* const* out, int numOut, int numSamples); // audio thread
    double getPreviewPosition() const noexcept { return previewPos.load(); }

    // ---------------- model (message thread) ----------------
    bool hasTake() const;
    std::shared_ptr<const Analysis> getAnalysis() const;
    juce::int64 getTakeStart() const noexcept { return takeStart.load(); }
    double getTakeSampleRate() const;
    double getTakeRate() const noexcept { return takeRate.load(); }       // lock-free, for the audio thread
    int getTakeLength() const noexcept { return takeLength.load(); }
    int getTakeChannels() const noexcept { return takeChannels.load(); }
    std::vector<Note> getNotes() const;
    void setNotes (std::vector<Note> notes);        // marks the render dirty
    void pushUndo();                                 // snapshot current notes before an edit
    bool undo();
    bool redo();
    bool canUndo() const;
    bool canRedo() const;
    void clearTake();

    juce::uint32 getModelVersion() const noexcept { return modelVersion.load(); }   // analysis or notes changed
    juce::uint32 getRenderVersion() const noexcept { return renderVersion.load(); }
    bool isAnalysing() const noexcept { return analysing.load(); }
    float getAnalysisProgress() const noexcept { return progress.load(); }

    juce::Image getSpectrogram (double& hopSamples, float& minMidi, float& maxMidi) const;

    // ---------------- persistence ----------------
    juce::ValueTree saveState() const;
    void loadState (const juce::ValueTree& tree);

    // Synchronous helpers (tests / offline).
    void loadTakeBlocking (std::vector<std::vector<float>> audio, double sampleRate, juce::int64 timelineStart);
    void loadTakeBlocking (std::vector<float> mono, double sampleRate, juce::int64 timelineStart);
    void renderBlocking();
    std::vector<std::vector<float>> getRenderedCopy() const;
    bool waitUntilIdle (int timeoutMs);
    juce::Range<int> getLastRenderedRange() const noexcept { return lastRenderedRange; }

private:
    void run() override;
    void ensureCaptureChunks();
    void doAnalysis();
    void doRender();
    void doSpectrogram();
    void installTake (std::vector<std::vector<float>> audio, double sr, juce::int64 start, AnalysisResult result,
                      bool keepUndo = false);
    void doImport();
    void doSync();
    void resetSyncBuffer();
    std::vector<std::vector<float>> collectCapture (double& rate, juce::int64& origin, int& length);
    static std::vector<float> monoMix (const std::vector<std::vector<float>>& ch);

    // capture: fixed-size chunks allocated ahead of the write position by the worker thread
    static constexpr int chunkBits = 20, chunkSize = 1 << chunkBits, maxChunks = 1024;
    std::atomic<CaptureState> captureState { CaptureState::idle };
    std::array<std::vector<std::unique_ptr<float[]>>, 2> chunks;
    std::atomic<int> chunksReady { 0 };
    std::atomic<int> captureChannels { 1 };
    std::atomic<bool> inCaptureBlock { false };
    std::atomic<juce::int64> captureOrigin { -1 };
    std::atomic<int> captureLength { 0 };
    double captureRate = 44100.0;

    // serialises analysis installs, renders and state I/O against each other
    mutable juce::CriticalSection workLock;

    // take + render (audioLock guards buffer swaps; the audio thread only try-locks it)
    mutable juce::SpinLock audioLock;
    std::vector<std::vector<float>> take, front, back;
    std::atomic<juce::int64> takeStart { 0 };
    std::atomic<int> takeLength { 0 }, takeChannels { 0 };
    std::atomic<double> takeRate { 0.0 };
    std::atomic<double> previewPos { -1.0 }, previewEnd { 0.0 };

    // render bookkeeping (worker thread)
    std::vector<Note> renderedNotes;
    bool fullRenderNeeded = true;
    juce::Range<int> staleInBack;          // region where `back` lags behind `front`
    juce::Range<int> lastRenderedRange;
    juce::Range<int> spectrumDirty;

    // model
    mutable juce::CriticalSection modelLock;
    std::shared_ptr<const Analysis> analysis;
    std::vector<Note> notes;
    std::vector<std::vector<Note>> undoStack, redoStack;
    std::atomic<juce::uint32> modelVersion { 1 }, renderVersion { 0 }, editVersion { 0 };
    juce::uint32 renderedEdit = 0, spectrumEdit = 0;
    std::atomic<juce::uint32> lastEditMs { 0 };

    mutable juce::CriticalSection specLock;
    juce::Image spectrogram;
    SpectrogramData specData;
    double specHop = 512.0;
    float specMin = 24.0f, specMax = 108.0f;

    // import + automatic alignment
    mutable juce::CriticalSection importLock;
    juce::File pendingImport;
    juce::String importedName, statusText;
    std::atomic<bool> importPending { false }, synced { true }, syncWanted { false }, syncReady { false }, inSyncFeed { false };
    std::vector<float> syncBuffer;              // mono, host rate; allocated before syncWanted is set
    std::atomic<int> syncCount { 0 };
    std::atomic<juce::int64> syncStart { 0 }, syncNext { -1 };
    std::atomic<double> syncHostRate { 44100.0 };
    int syncAttempts = 0;

    std::atomic<bool> analysing { false }, cancelAnalysis { false };
    std::atomic<float> progress { 0.0f };
};
} // namespace nf
