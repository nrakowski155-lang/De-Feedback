#pragma once

#include <chrono>
#include <cstdint>

namespace defb::ai
{

// ---------------------------------------------------------------------------
// Model contract shared by the DSP engine, the built-in baseline scorer and
// the Core ML bridge. The numbers are fixed: the training pipeline exports a
// model with exactly this input shape and these output names.
//
//   input  "spectrogram"  float32 [1, 1, 64, 256]   (band-major, newest last)
//   output "mask"         float32 [1, 64]           feedback probability / band
//   output "clip"         float32 [1, 1]            feedback probability / clip
//
// Values are normalised dB: v = clamp ((dB + 100) / 100, 0, 1), i.e. -100 dBFS
// maps to 0 and 0 dBFS maps to 1 (see Engine::analyzeFrame and training/).
// ---------------------------------------------------------------------------
inline constexpr int kNumBands    = 64;
inline constexpr int kPatchFrames = 256; // 256 hops x 256 samples = 1.37 s @ 48 kHz
inline constexpr int kPatchSize   = kNumBands * kPatchFrames;

inline constexpr int kModelNone    = 0;
inline constexpr int kModelBuiltin = 1;
inline constexpr int kModelCoreMl  = 2;

// A mask older than this is ignored, so the detector falls back to pure DSP
// when the worker thread stalls or the host suspends processing.
inline constexpr float kMaskStaleMs = 500.0f;

// The model may only *strengthen* a detection. Confidence below the floor is
// neutral (never a veto), because an unvalidated model must not be able to
// suppress a real howl. Above the floor it adds up to +8 dB of prominence and
// above the trust threshold it can overrule the music guard.
inline constexpr float kAiBoostFloor   = 0.55f;
inline constexpr float kAiBoostDb      = 8.0f;
inline constexpr float kAiTrustThreshold = 0.70f;

inline constexpr float kDefaultLowBandHz  = 100.0f;
inline constexpr float kDefaultHighBandHz = 16000.0f;

struct Mask
{
    float confidence[kNumBands] {};
    float clipProbability = 0.5f;
    int   model           = kModelNone;
    int64_t timestampMs   = 0;
};

inline int64_t nowMs() noexcept
{
    using namespace std::chrono;
    return duration_cast<milliseconds> (steady_clock::now().time_since_epoch()).count();
}

} // namespace defb::ai
