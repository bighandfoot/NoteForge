#pragma once
#include "model/NoteModel.h"
#include <atomic>

namespace nf
{
struct AnalysisResult
{
    Analysis analysis;
    std::vector<Note> notes;
};

int analysisHopFor (double sampleRate);
extern double offlineWindowMs; // pitch-tracker window used for transferred takes

// Full offline analysis of a mono take: f0 tracking, cleanup, pitch marks and note segmentation.
AnalysisResult analyzeTake (const float* x, int numSamples, double sampleRate,
                            std::atomic<float>* progress = nullptr, const std::atomic<bool>* cancel = nullptr);

// Fills rms/rawHz/aperiodicity for frames [k0, k1) (arrays must already be sized).
void computeFrames (Analysis& a, const float* x, int k0, int k1,
                    std::atomic<float>* progress = nullptr, const std::atomic<bool>* cancel = nullptr);
// Voicing, cleanup, pitch marks and note segmentation from the raw frame data.
std::vector<Note> finalizeAnalysis (Analysis& a, const float* x);

void computePitchMarks (Analysis& a, const float* x);
std::vector<Note> segmentNotes (const Analysis& a);
} // namespace nf
