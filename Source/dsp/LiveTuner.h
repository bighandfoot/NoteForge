#pragma once
#include "PitchDetector.h"
#include "model/Scale.h"
#include <array>
#include <atomic>
#include <vector>

namespace nf
{
// Real-time scale-snapping pitch corrector ("auto-tune" mode). Two engines share one pitch tracker:
//  - high quality: streaming PSOLA (formant-preserving, formant control), delay of ~4 pitch periods;
//  - low latency:  pitch-synchronous variable delay line (classic hardware auto-tune style), delay of
//                  ~half a pitch period, for monitoring while recording. Formant control is not available.
// Mono or stereo: pitch is tracked on the mix and both channels get identical processing.
class LiveTuner
{
public:
    struct Params
    {
        Scale scale;
        float retuneMs = 20.0f; // 0 = instant (hard-tune effect)
        float amount = 1.0f;    // 0..1 how far towards the target note
        float formant = 0.0f;   // semitones (high-quality engine only)
        float transpose = 0.0f; // semitones, applied after correction
        float mix = 1.0f;
        bool lowLatency = false;
        bool bypass = false;    // hear the untuned voice (same delay, so A/B comparisons line up)
    };

    // lowestHz: lowest pitch to track; both engines' delay scales with its period.
    void prepare (double sampleRate, float lowestHz = 90.0f);
    void reset();
    // The host skipped some audio (e.g. FL 'smart disable' paused us): drop everything in flight so no stale
    // fragments are glued onto the new audio. Treats the gap as silence. Real-time safe (no allocation).
    void resetAfterGap();
    int latencySamples (bool lowLatency = false) const noexcept { return lowLatency ? latencyLow : latencyHigh; }

    // Lowest pitch tracked by the "Auto" voice range: covers any singer from about B1 upwards.
    static constexpr float autoRangeHz = 60.0f;
    // Counts pitched, loud frames that were below the selected range (and therefore left untuned).
    std::atomic<int> notesBelowRange { 0 };
    // Diagnostics: analysis frames processed / judged pitched (voiced).
    std::atomic<int> framesAnalysed { 0 }, voicedFrameCount { 0 };
    // Grains that reached back into audio already sent to the host (would be heard as a click / crackle).
    std::atomic<int> lateGrains { 0 };

    // 1 or 2 channels; `in` and `out` may alias.
    void process (const float* const* in, float* const* out, int numChannels, int numSamples, const Params& p);
    void process (const float* monoIn, float* monoOut, int numSamples, const Params& p)
    {
        const float* i[] = { monoIn };
        float* o[] = { monoOut };
        process (i, o, 1, numSamples, p);
    }

    // Pitch history for the UI (written on the audio thread, read on the message thread).
    static constexpr int historySize = 512;
    std::array<std::atomic<float>, historySize> historyIn {}, historyOut {};
    std::atomic<int> historyWrite { 0 };

private:
    struct Mark { long long pos; bool voiced; };

    // Fixed-capacity FIFO of pitch marks. Never allocates, so it is safe on the audio thread
    // (std::deque allocates a block for nearly every element with MSVC, which caused crackles).
    class MarkRing
    {
    public:
        void clear() noexcept { head = 0; count = 0; }
        size_t size() const noexcept { return count; }
        bool empty() const noexcept { return count == 0; }
        void push_back (Mark m) noexcept
        {
            if (count == capacity) pop_front(); // cannot happen in practice; drop the oldest rather than grow
            items[(head + count) & (capacity - 1)] = m;
            ++count;
        }
        void pop_front() noexcept { if (count > 0) { head = (head + 1) & (capacity - 1); --count; } }
        Mark& operator[] (size_t i) noexcept { return items[(head + i) & (capacity - 1)]; }
        const Mark& operator[] (size_t i) const noexcept { return items[(head + i) & (capacity - 1)]; }
        Mark& back() noexcept { return (*this)[count - 1]; }
    private:
        static constexpr size_t capacity = 4096;
        std::array<Mark, capacity> items {};
        size_t head = 0, count = 0;
    };

    void analyseFrames();
    void placeMarks();
    void synthesise (const Params& p, int numCh);
    void renderLowLatency (float* const* out, int numCh, int n, long long blockStart, const Params& p);
    float targetShift (float detected, const Params& p); // semitones towards the target note (+ transpose)
    void pushHistory (float detected, float corrected);
    float frameMidi (double frame) const;
    bool frameVoiced (long long frame) const;
    float readAt (int ch, double pos) const;

    double sr = 44100.0;
    int hop = 256, latencyHigh = 4096, latencyLow = 512, maxPeriod = 800, unvoicedStep = 220;
    PitchDetector detector, probe; // probe: wide-range check for notes below the selected range
    float lowest = 90.0f;
    bool probing = false;
    int fillCount = 0;
    bool onsetPending = false;
    float onsetMidi = 0.0f;

    static constexpr int ringBits = 17;
    static constexpr long long ringSize = 1ll << ringBits, ringMask = ringSize - 1;
    std::vector<float> in, lp, wsum;              // `in` = mono mix used for tracking
    std::array<std::vector<float>, 2> inCh, accCh;
    long long written = 0;
    float lp1 = 0.0f, lp2 = 0.0f, lpAlpha = 0.9f;

    static constexpr int frameRing = 4096;
    std::vector<float> frames;
    std::vector<uint8_t> framesVoiced;
    long long nextFrame = 0;
    std::vector<float> scratch;

    // high-quality (PSOLA) state
    MarkRing marks;
    bool lastMarkVoiced = false;
    double outMark = 0.0;
    long long lastOutput = -1; // last sample already handed to the host (high-quality engine)

    // low-latency (delay line) state
    double delay = 0.0, oldDelay = 0.0, lastPeriod = 300.0;
    int fadePos = 0, fadeLen = 0;
    float llShift = 0.0f;

    float smoothShift = 0.0f;
    float mixNow = 1.0f, mixAlpha = 0.01f; // wet amount, smoothed so bypass/mix changes never click
    int currentTarget = -1000;
    int historyDecim = 0;
};
} // namespace nf
