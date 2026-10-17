#pragma once

namespace nf
{
// Finds where `chunk` (audio heard while the host plays) occurs inside `take` (an imported file).
// offset = index in `take` that lines up with chunk[0] (negative if the chunk starts before the take).
// confidence = normalised cross-correlation at that offset (1 = identical, < ~0.5 = not found).
struct AlignResult
{
    long long offset = 0;
    float confidence = 0.0f;
};

AlignResult findChunkInTake (const float* take, int takeLength, const float* chunk, int chunkLength);
} // namespace nf
