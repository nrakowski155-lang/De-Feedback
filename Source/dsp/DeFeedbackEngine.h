#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <vector>

#include "../ai/AiTypes.h"
#include "../ai/SpscQueue.h"
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
    int   maxNotches    = 8;      // 1 .. 12
    float depthDb       = 18.0f;  // notch depth (adaptive: 80 .. 100 % of this)
    float q             = 16.0f;  // manual notch width, or upper bound when autoQ
    float attackMs      = 60.0f;  // how long a peak must persist before it is cut
                                  // (a dominant, climbing howl is cut sooner: see
                                  //  the fast lane in analyzeFrame)
    float releaseMs     = 2500.0f;// dynamic mode only
    float lowFreq       = 40.0f;
    float highFreq      = 16000.0f;
    bool  musicGuard    = true;   // require a rising envelope before cutting
    bool  aiAssist      = true;   // use the ResonanceAI band mask as extra evidence
    bool  autoQ         = true;   // derive the notch Q from the measured peak width
    Mode  mode          = Mode::Hunt;
};

struct NotchInfo
{
    float freq    = 0.0f;
    float gainDb  = 0.0f;
    float levelDb = -120.0f;
    float q       = 16.0f;
};

struct AiState
{
    bool     maskFresh     = false;
    float    confidence    = 0.0f;
    int      model         = ai::kModelNone;
    uint32_t patchRevision = 0;
};

// Adaptive feedback suppressor with an AI-assisted decision stage.
//
// DSP path (always active, 0 samples of added latency):
//   1. short-time spectrum (2048 pt, hop 256 => 5.33 ms at 48 kHz)
//   2. local spectral floor (fractional-octave average)
//   3. narrow peak picking with parabolic frequency interpolation
//   4. 4096 pt confirmation FFT: exact frequency and measured peak width
//      (the width drives the automatic notch Q, the 12 notches are dynamic)
//   5. multi-frame tracker: persistence + rising envelope => feedback, not a note
//   6. one peaking notch per detected frequency, linked stereo
//
// AI path (optional, asynchronous):
//   * the engine publishes a 64 x 256 log-band patch (seqlock, no allocation)
//   * the AI worker copies the patch, runs the model and posts a band mask back
//     through a lock-free SPSC queue
//   * the mask can only add evidence (+0 .. +8 dB prominence) and can overrule
//     the music guard when very confident; missing or stale masks fall back to
//     pure DSP. A model can never veto a detection.
class Engine
{
public:
    static constexpr int kOrder       = 11;
    static constexpr int kFftSize     = 1 << kOrder;   // 2048
    static constexpr int kHop         = 256;           // 5.33 ms at 48 kHz
    static constexpr int kNumBins     = kFftSize / 2 + 1;
    static constexpr int kFineOrder   = 12;
    static constexpr int kFineSize    = 1 << kFineOrder; // 4096
    static constexpr int kFineBins    = kFineSize / 2 + 1;
    static constexpr int kNumBands    = ai::kNumBands;
    static constexpr int kPatchFrames = ai::kPatchFrames;
    static constexpr int kPatchSize   = ai::kPatchSize;
    static constexpr int kMaxTracks   = 32;
    static constexpr int kMaxNotches  = 12;
    static constexpr int kDisplayBins = 1024;
    static constexpr int kConfirmEvery = 4;            // ~21 ms at 48 kHz: exact
                                                       // frequency and Q early
    static constexpr float kMinAutoQ   = 10.0f;        // narrow, voice-safe notches
    static constexpr float kMaxAutoQ   = 30.0f;

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

    // ---- ResonanceAI worker interface -------------------------------------
    // postAiMask: AI worker thread only. copyAiPatch: AI worker thread only.
    void     postAiMask (const ai::Mask& mask) noexcept;
    int      copyAiPatch (float* out, int maxFloats) const noexcept;
    uint32_t getAiPatchRevision() const noexcept
    {
        return patchRevision_.load (std::memory_order_acquire);
    }
    AiState  getAiState() const noexcept;
    int      getBandForFrequency (float freq) const noexcept;

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
        float q            = 0.0f; // measured peak Q (0 = not measured yet)
        float aiConfidence = 0.0f;
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
        float q          = 16.0f;
        int   track      = -1;
        int64_t age      = 0;
        BiquadCoeffs coeffs;
        BiquadState  state[2];
    };

    struct Candidate
    {
        float freq         = 0.0f;
        float levelDb      = -120.0f;
        float prominence   = 0.0f;
        float aiConfidence = 0.0f;
    };

    void analyzeFrame() noexcept;
    void pushSample (float s) noexcept;
    void refineTracks() noexcept;
    void fillPatchFrame() noexcept;
    void publishPatch() noexcept;
    void absorbAiMasks() noexcept;
    void allocateNotch (int trackIndex) noexcept;
    void freeNotch (int notchIndex) noexcept;
    void publishSnapshot() noexcept;

    double sampleRate_ = 48000.0;
    int    maxBlock_   = 512;
    Parameters params_;

    Fft fft_ { kOrder };
    Fft fftFine_ { kFineOrder };

    std::array<float, kFftSize> window_ {};
    std::array<float, kFftSize> hist_ {};
    std::array<float, kFftSize> re_ {};
    std::array<float, kFftSize> im_ {};
    std::array<float, kNumBins> db_ {};
    std::array<float, kNumBins> floorDb_ {};
    std::array<float, kNumBins + 2> prefix_ {};

    std::array<float, kFineSize> windowFine_ {};
    std::array<float, kFineSize> histFine_ {};
    std::array<float, kFineSize> reFine_ {};
    std::array<float, kFineSize> imFine_ {};
    std::array<float, kFineBins> fineDb_ {};

    std::array<int, kNumBands> bandLo_ {};
    std::array<int, kNumBands> bandHi_ {};
    std::array<float, kNumBands> bandFreq_ {};

    std::array<std::array<float, kNumBands>, kPatchFrames> patch_ {};
    std::array<float, kPatchSize> patchSnapshot_[2] {};

    std::array<Candidate, 64> candidates_ {};

    int histPos_      = 0;
    int histFinePos_  = 0;
    int hopCount_     = 0;
    int frameCount_   = 0;
    int patchWrite_   = 0;
    bool clearing_    = false;

    std::array<Track, kMaxTracks> tracks_ {};
    std::array<Notch, kMaxNotches> notches_ {};

    // ---- AI worker communication ----
    ai::SpscQueue<ai::Mask, 8> aiQueue_ {};
    ai::Mask latestMask_ {};
    bool     haveMask_    = false;
    bool     maskFresh_   = false;
    int64_t  maskStampMs_ = 0;
    float    aiConfidence_ = 0.0f;
    int      aiModel_      = ai::kModelNone;

    std::atomic<uint32_t> patchSeq_ { 0 };
    std::atomic<int>      patchIndex_ { 0 };
    std::atomic<uint32_t> patchRevision_ { 0 };

    // ---- snapshot for the GUI (seqlock: audio thread writes, UI reads) ----
    struct Snapshot
    {
        float spectrum[kDisplayBins] {};
        float floorDb[kDisplayBins] {};
        float notchFreq[kMaxNotches] {};
        float notchGain[kMaxNotches] {};
        float notchQ[kMaxNotches] {};
        int   numNotches = 0;
        int   hunting    = 0;
        float sensitivity = 9.0f;
        float maxFreq    = 20000.0f;
        int   aiFresh    = 0;
        int   aiModel    = 0;
        float aiConfidence = 0.0f;
    };

    Snapshot snapshot_[2] {};
    std::atomic<uint32_t> snapshotSeq_ { 0 };
    std::atomic<int> snapshotIndex_ { 0 };
    int snapshotCounter_ = 0;
};

} // namespace defb
