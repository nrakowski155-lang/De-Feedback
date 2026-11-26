#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <vector>

#include "Biquad.h"
#include "Fft.h"

namespace defb
{

enum class Mode
{
    Off     = 0,
    Hunt    = 1, // detect, latch notches in place (live / stage)
    Dynamic = 2  // detect, release notches again when the howl is gone (studio)
};

struct Parameters
{
    float sensitivityDb = 9.0f;   // how far a peak must stick out of the local spectrum
    int   maxNotches    = 8;      // 1 .. 16
    float depthDb       = 18.0f;  // notch depth
    float q             = 16.0f;  // notch width (higher = narrower)
    float attackMs      = 250.0f; // how long a peak must persist before it is cut
    float releaseMs     = 2500.0f;// dynamic mode only
    float lowFreq       = 40.0f;
    float highFreq      = 16000.0f;
    bool  musicGuard    = true;   // require a rising envelope before cutting
    Mode  mode          = Mode::Hunt;
};

struct NotchInfo
{
    float freq    = 0.0f;
    float gainDb  = 0.0f;
    float levelDb = -120.0f;
};

// Adaptive feedback suppressor.
//
// Strategy (mirrors the way Feedback Hunter / De-Feedback work in practice):
//   1. short-time spectrum (2048 pt, 75 % overlap)
//   2. local spectral floor (fractional-octave average)
//   3. narrow peak picking with parabolic frequency interpolation
//   4. multi-frame tracker: persistence + rising envelope => feedback, not a note
//   5. one narrow peaking notch per detected frequency, linked stereo
//
// The core is framework-free so it can be compiled and verified on its own.
class Engine
{
public:
    static constexpr int kOrder       = 11;
    static constexpr int kFftSize     = 1 << kOrder; // 2048
    static constexpr int kHop         = 512;
    static constexpr int kNumBins     = kFftSize / 2 + 1;
    static constexpr int kMaxTracks   = 32;
    static constexpr int kMaxNotches  = 16;
    static constexpr int kDisplayBins = 1024;

    Engine();
    ~Engine() = default;

    void prepare (double sampleRate, int maxBlockSize);
    void reset();

    void setParameters (const Parameters& p);
    Parameters getParameters() const noexcept { return params_; }

    // Interleaved processing is not used: pass channel pointers.
    // right may be nullptr for mono.
    void process (float* left, float* right, int numSamples) noexcept;

    // Releases every notch (short fade, no click) and forgets the learned set.
    void clearNotches() noexcept;

    int  getNotchInfo (NotchInfo* out, int maxOut) const noexcept;
    int  getNumNotches() const noexcept;
    bool isAnalyzing() const noexcept { return params_.mode != Mode::Off; }

    // Spectrum snapshot for the editor (dB, 0 .. kDisplayBins-1).
    int   getDisplaySpectrum (float* out, int maxBins, float* floorOut = nullptr) const noexcept;
    float getDisplaySampleRate() const noexcept { return (float) sampleRate_; }
    static float displayBinToFreq (int bin, double sampleRate) noexcept
    {
        return (float) ((double) bin * sampleRate / (double) kFftSize);
    }

private:
    struct Track
    {
        bool  active       = false;
        float freq         = 0.0f;
        float levelDb      = -120.0f;
        float prominence   = 0.0f;
        float startLevelDb = -120.0f;
        float evidence     = 0.0f;
        float rise         = 0.0f; // dB per analysis frame, smoothed
        bool  grown        = false;
        int   lastSeen     = 0;
        int   notch        = -1;
        float levelHist[32] {};
        int   histPos      = 0;
        int   histCount    = 0;
    };

    struct Notch
    {
        bool  active     = false;
        float freq       = 0.0f;
        float gainDb     = 0.0f;
        float targetDb   = 0.0f;
        float levelDb    = -120.0f;
        int   track      = -1;
        int64_t age      = 0;
        BiquadCoeffs coeffs;
        BiquadState  state[2];
    };

    struct Candidate
    {
        float freq       = 0.0f;
        float levelDb    = -120.0f;
        float prominence = 0.0f;
    };

    void analyzeFrame() noexcept;
    void pushSample (float s) noexcept;
    void updateNotchTargets() noexcept;
    void allocateNotch (int trackIndex) noexcept;
    void freeNotch (int notchIndex) noexcept;
    void publishSnapshot() noexcept;

    double sampleRate_ = 48000.0;
    int    maxBlock_   = 512;
    Parameters params_;

    Fft fft_ { kOrder };
    std::array<float, kFftSize> window_ {};
    std::array<float, kFftSize> hist_ {};
    std::array<float, kFftSize> re_ {};
    std::array<float, kFftSize> im_ {};
    std::array<float, kNumBins> db_ {};
    std::array<float, kNumBins> floorDb_ {};
    std::array<float, kNumBins + 2> prefix_ {};
    std::array<Candidate, 64> candidates_ {};

    int histPos_    = 0;
    int hopCount_   = 0;
    int frameCount_ = 0;
    bool clearing_  = false;

    std::array<Track, kMaxTracks> tracks_ {};
    std::array<Notch, kMaxNotches> notches_ {};

    // ---- snapshot for the GUI (seqlock: audio thread writes, UI reads) ----
    struct Snapshot
    {
        float spectrum[kDisplayBins] {};
        float floorDb[kDisplayBins] {};
        float notchFreq[kMaxNotches] {};
        float notchGain[kMaxNotches] {};
        int   numNotches = 0;
        int   hunting    = 0;
        float sensitivity = 9.0f;
        float maxFreq    = 20000.0f;
    };

    Snapshot snapshot_[2] {};
    std::atomic<uint32_t> snapshotSeq_ { 0 };
    std::atomic<int> snapshotIndex_ { 0 };
    int snapshotCounter_ = 0;
};

} // namespace defb
