#include "DeFeedbackEngine.h"

namespace defb
{

namespace
{
    inline float normalisedDb (float db) noexcept
    {
        return std::clamp ((db + 100.0f) / 100.0f, 0.0f, 1.0f);
    }

    inline double mel (double f) noexcept
    {
        return 2595.0 * std::log10 (1.0 + f / 700.0);
    }

    inline double inverseMel (double m) noexcept
    {
        return 700.0 * (std::pow (10.0, m / 2595.0) - 1.0);
    }
}

Engine::Engine()
{
    prepare (48000.0, 512);
}

void Engine::prepare (double sampleRate, int maxBlockSize)
{
    sampleRate_ = sampleRate > 0.0 ? sampleRate : 48000.0;
    maxBlock_   = std::max (1, maxBlockSize);

    // Hann analysis windows
    for (int i = 0; i < kFftSize; ++i)
    {
        const double w = 0.5 - 0.5 * std::cos (2.0 * Fft::kPi * (double) i / (double) (kFftSize - 1));
        window_[(size_t) i] = (float) w;
    }

    for (int i = 0; i < kFineSize; ++i)
    {
        const double w = 0.5 - 0.5 * std::cos (2.0 * Fft::kPi * (double) i / (double) (kFineSize - 1));
        windowFine_[(size_t) i] = (float) w;
    }

    // 64 mel/log bands from 100 Hz to min(16 kHz, 0.45 fs) - the AI model input.
    const double fLow  = std::min ((double) ai::kDefaultLowBandHz, sampleRate_ * 0.4);
    const double fHigh = std::min ((double) ai::kDefaultHighBandHz, sampleRate_ * 0.45);
    const double mLow  = mel (fLow);
    const double mHigh = mel (fHigh);
    const double binHz = sampleRate_ / (double) kFftSize;

    std::array<double, kNumBands + 1> edges {};

    for (int b = 0; b <= kNumBands; ++b)
        edges[(size_t) b] = inverseMel (mLow + (mHigh - mLow) * (double) b / (double) kNumBands);

    for (int b = 0; b < kNumBands; ++b)
    {
        const int lo = std::clamp ((int) std::floor (edges[(size_t) b] / binHz), 1, kNumBins - 2);
        const int hi = std::clamp ((int) std::ceil (edges[(size_t) b + 1] / binHz), lo, kNumBins - 2);

        bandLo_[(size_t) b]   = lo;
        bandHi_[(size_t) b]   = hi;
        bandFreq_[(size_t) b] = (float) (0.5 * (edges[(size_t) b] + edges[(size_t) b + 1]));
    }

    reset();
}

void Engine::reset()
{
    hist_.fill (0.0f);
    histFine_.fill (0.0f);
    re_.fill (0.0f);
    im_.fill (0.0f);
    reFine_.fill (0.0f);
    imFine_.fill (0.0f);
    db_.fill (-120.0f);
    floorDb_.fill (-120.0f);
    fineDb_.fill (-120.0f);
    prefix_.fill (0.0f);

    for (auto& frame : patch_)
        frame.fill (0.0f);

    histPos_     = 0;
    histFinePos_ = 0;
    hopCount_    = 0;
    frameCount_  = 0;
    patchWrite_  = 0;
    clearing_    = false;

    for (auto& t : tracks_)
        t = Track {};

    for (auto& n : notches_)
    {
        n = Notch {};
        n.state[0].reset();
        n.state[1].reset();
    }

    aiQueue_.clear();
    latestMask_   = ai::Mask {};
    haveMask_     = false;
    maskFresh_    = false;
    maskStampMs_  = 0;
    aiConfidence_ = 0.0f;
    aiModel_      = ai::kModelNone;

    publishPatch();
    publishSnapshot();
}

void Engine::setParameters (const Parameters& p)
{
    const bool modeChanged = (p.mode != params_.mode);
    params_                = p;

    params_.sensitivityDb = std::clamp (params_.sensitivityDb, 3.0f, 30.0f);
    params_.maxNotches    = std::clamp (params_.maxNotches, 1, (int) kMaxNotches);
    params_.depthDb       = std::clamp (params_.depthDb, 0.0f, 36.0f);
    params_.q             = std::clamp (params_.q, 2.0f, 48.0f);
    params_.attackMs      = std::clamp (params_.attackMs, 10.0f, 4000.0f);
    params_.releaseMs     = std::clamp (params_.releaseMs, 100.0f, 60000.0f);
    params_.lowFreq       = std::clamp (params_.lowFreq, 20.0f, 1000.0f);
    params_.highFreq      = std::clamp (params_.highFreq, 1000.0f, (float) (sampleRate_ * 0.48));

    if (params_.lowFreq > params_.highFreq * 0.5f)
        params_.lowFreq = params_.highFreq * 0.5f;

    if (modeChanged)
    {
        // Never carry stale filter state into a new mode.
        for (auto& n : notches_)
        {
            n = Notch {};
            n.state[0].reset();
            n.state[1].reset();
        }
        for (auto& t : tracks_)
            t = Track {};
        clearing_ = false;
    }

    // A reduced notch budget releases the surplus filters immediately.
    int used = 0;
    for (auto& n : notches_)
    {
        if (! n.active)
            continue;
        if (++used > params_.maxNotches)
        {
            if (n.track >= 0)
                tracks_[(size_t) n.track].notch = -1;
            n.targetDb = 0.0f;
            n.track    = -1;
        }
    }

    if (modeChanged)
        publishSnapshot();
}

void Engine::clearNotches() noexcept
{
    for (auto& t : tracks_)
        t = Track {};

    for (auto& n : notches_)
    {
        if (n.active)
        {
            n.targetDb = 0.0f;
            n.track    = -1;
        }
    }

    clearing_ = true;
}

void Engine::pushSample (float s) noexcept
{
    hist_[(size_t) histPos_] = s;
    histPos_ = (histPos_ + 1) % kFftSize;

    histFine_[(size_t) histFinePos_] = s;
    histFinePos_ = (histFinePos_ + 1) % kFineSize;
}

void Engine::process (float* left, float* right, int numSamples) noexcept
{
    if (left == nullptr || numSamples <= 0)
        return;

    if (params_.mode == Mode::Off)
        return; // untouched, bit-exact bypass

    // ---- 1. analysis of the incoming (pre-notch) signal ----
    int offset    = 0;
    int remaining = numSamples;

    while (remaining > 0)
    {
        const int chunk = std::min (remaining, kHop - hopCount_);

        // listen to the mono sum, so a howl that only appears on one side is
        // still detected (the notches themselves stay linked stereo)
        if (right != nullptr)
            for (int i = 0; i < chunk; ++i)
                pushSample (0.5f * (left[offset + i] + right[offset + i]));
        else
            for (int i = 0; i < chunk; ++i)
                pushSample (left[offset + i]);

        hopCount_ += chunk;
        offset    += chunk;
        remaining -= chunk;

        if (hopCount_ >= kHop)
        {
            hopCount_ = 0;
            analyzeFrame();
        }
    }

    // ---- 2. notch gain smoothing + coefficient update (block rate) ----
    const float blockSec      = (float) numSamples / (float) sampleRate_;
    const float engageCoeff   = 1.0f - std::exp (-blockSec / 0.030f);
    const float releaseCoeff  = 1.0f - std::exp (-blockSec / 0.080f);

    int activeCount = 0;

    for (auto& n : notches_)
    {
        if (! n.active)
            continue;

        const float coeff = n.targetDb < n.gainDb ? engageCoeff : releaseCoeff;
        n.gainDb += (n.targetDb - n.gainDb) * coeff;

        if (std::abs (n.targetDb - n.gainDb) < 0.05f)
            n.gainDb = n.targetDb;

        if (n.gainDb > -0.02f && n.targetDb >= 0.0f)
        {
            // fully released -> give the slot back
            n.active = false;
            n.track  = -1;
            n.state[0].reset();
            n.state[1].reset();
            continue;
        }

        n.coeffs = makePeaking (sampleRate_, (double) n.freq, (double) n.q, (double) n.gainDb);
        ++activeCount;
    }

    if (clearing_ && activeCount == 0)
        clearing_ = false;

    // ---- 3. filtering ----
    if (activeCount > 0)
    {
        int idx[kMaxNotches];
        int count = 0;

        for (int i = 0; i < (int) kMaxNotches; ++i)
            if (notches_[(size_t) i].active)
                idx[count++] = i;

        if (right != nullptr)
        {
            for (int s = 0; s < numSamples; ++s)
            {
                float l = left[s];
                float r = right[s];

                for (int k = 0; k < count; ++k)
                {
                    auto& n = notches_[(size_t) idx[k]];
                    l = processBiquad (n.coeffs, n.state[0], l);
                    r = processBiquad (n.coeffs, n.state[1], r);
                }

                left[s]  = l;
                right[s] = r;
            }
        }
        else
        {
            for (int s = 0; s < numSamples; ++s)
            {
                float l = left[s];

                for (int k = 0; k < count; ++k)
                {
                    auto& n = notches_[(size_t) idx[k]];
                    l = processBiquad (n.coeffs, n.state[0], l);
                }

                left[s] = l;
            }
        }
    }

    // ---- 4. GUI snapshot at ~20 Hz ----
    if (++snapshotCounter_ >= 4)
    {
        snapshotCounter_ = 0;
        publishSnapshot();
    }
}

void Engine::absorbAiMasks() noexcept
{
    ai::Mask incoming;

    while (aiQueue_.pop (incoming))
    {
        latestMask_  = incoming;
        haveMask_    = true;
        maskStampMs_ = incoming.timestampMs;
    }

    maskFresh_ = haveMask_
              && (ai::nowMs() - maskStampMs_) <= (int64_t) ai::kMaskStaleMs;

    if (! haveMask_)
    {
        aiConfidence_ = 0.0f;
        aiModel_      = ai::kModelNone;
        return;
    }

    aiModel_ = latestMask_.model;

    if (! maskFresh_)
    {
        aiConfidence_ = 0.0f;
        return;
    }

    float peak = 0.0f;
    for (int b = 0; b < kNumBands; ++b)
        peak = std::max (peak, std::clamp (latestMask_.confidence[b], 0.0f, 1.0f));

    aiConfidence_ = peak;
}

void Engine::fillPatchFrame() noexcept
{
    auto& frame = patch_[(size_t) patchWrite_];

    for (int b = 0; b < kNumBands; ++b)
    {
        float level = -120.0f;

        for (int k = bandLo_[(size_t) b]; k <= bandHi_[(size_t) b]; ++k)
            level = std::max (level, db_[(size_t) k]);

        frame[(size_t) b] = normalisedDb (level);
    }

    patchWrite_ = (patchWrite_ + 1) % kPatchFrames;
}

void Engine::publishPatch() noexcept
{
    const uint32_t seq = patchSeq_.load (std::memory_order_relaxed);
    patchSeq_.store (seq + 1, std::memory_order_release); // odd: writing

    auto& dst = patchSnapshot_[patchIndex_.load (std::memory_order_relaxed)];

    for (int t = 0; t < kPatchFrames; ++t)
    {
        const int src = (patchWrite_ + t) % kPatchFrames;

        for (int b = 0; b < kNumBands; ++b)
            dst[(size_t) b * (size_t) kPatchFrames + (size_t) t] = patch_[(size_t) src][(size_t) b];
    }

    patchSeq_.store (seq + 2, std::memory_order_release); // even: readable
    patchIndex_ ^= 1;
    patchRevision_.fetch_add (1, std::memory_order_release);
}

int Engine::copyAiPatch (float* out, int maxFloats) const noexcept
{
    if (out == nullptr || maxFloats <= 0)
        return 0;

    const int n = std::min (maxFloats, kPatchSize);

    for (int attempt = 0; attempt < 4; ++attempt)
    {
        const uint32_t s1 = patchSeq_.load (std::memory_order_acquire);
        if (s1 & 1u)
            continue;

        const auto& src = patchSnapshot_[patchIndex_.load (std::memory_order_relaxed) ^ 1];

        std::memcpy (out, src.data(), sizeof (float) * (size_t) n);

        const uint32_t s2 = patchSeq_.load (std::memory_order_acquire);
        if (s1 == s2)
            return n;
    }

    return 0;
}

void Engine::postAiMask (const ai::Mask& mask) noexcept
{
    aiQueue_.push (mask); // full queue: drop the message, never block
}

int Engine::getBandForFrequency (float freq) const noexcept
{
    if (freq <= bandFreq_[0])
        return 0;

    if (freq >= bandFreq_[(size_t) kNumBands - 1])
        return kNumBands - 1;

    int best = 0;
    float bestDistance = 1.0e9f;

    for (int b = 0; b < kNumBands; ++b)
    {
        const float distance = std::abs (std::log2 (freq / std::max (bandFreq_[(size_t) b], 1.0f)));

        if (distance < bestDistance)
        {
            bestDistance = distance;
            best         = b;
        }
    }

    return best;
}

void Engine::analyzeFrame() noexcept
{
    absorbAiMasks();

    const double binHz  = sampleRate_ / (double) kFftSize;
    const int    minBin = std::max (2, (int) std::ceil ((double) params_.lowFreq / binHz));
    const int    maxBin = std::min (kNumBins - 2, (int) std::floor ((double) params_.highFreq / binHz));

    if (maxBin <= minBin + 8)
        return;

    // windowed FFT of the newest kFftSize samples (+ programme level)
    float sumSquares = 0.0f;

    for (int i = 0; i < kFftSize; ++i)
    {
        const int idx = (histPos_ + i) % kFftSize;
        const float s = hist_[(size_t) idx];
        sumSquares += s * s;
        re_[(size_t) i] = s * window_[(size_t) i];
        im_[(size_t) i] = 0.0f;
    }

    const float rmsDb = 10.0f * std::log10 (sumSquares / (float) kFftSize + 1.0e-12f);

    fft_.forward (re_.data(), im_.data());

    const float norm = 4.0f / (float) kFftSize; // Hann coherent gain

    for (int k = 0; k < kNumBins; ++k)
    {
        const float re = re_[(size_t) k];
        const float im = im_[(size_t) k];
        const float mag = std::sqrt (re * re + im * im) * norm;
        const float db = 20.0f * std::log10 (mag + 1.0e-9f);
        db_[(size_t) k] = std::max (db, -120.0f);
    }

    fillPatchFrame();

    const bool confirmFrame = (frameCount_ % kConfirmEvery) == 0;

    if (confirmFrame)
    {
        refineTracks();
        publishPatch();
    }

    // fractional-octave floor (centre bins excluded so the peak cannot hide itself)
    prefix_[0] = 0.0f;
    for (int k = 0; k < kNumBins; ++k)
        prefix_[(size_t) k + 1] = prefix_[(size_t) k] + db_[(size_t) k];

    auto sumRange = [&] (int lo, int hi) -> float
    {
        lo = std::clamp (lo, 0, kNumBins - 1);
        hi = std::clamp (hi, lo, kNumBins - 1);
        return prefix_[(size_t) hi + 1] - prefix_[(size_t) lo];
    };

    for (int k = minBin; k <= maxBin; ++k)
    {
        // ~1/3 octave to each side, centre bins excluded so a peak cannot lift its own floor
        const int span = std::max (6, k / 3);
        const int lo   = std::max (0, k - span);
        const int hi   = std::min (kNumBins - 1, k + span);
        const int cl   = k - 3;
        const int ch   = k + 3;

        const int leftCount  = std::max (0, std::min (cl, hi + 1) - lo);
        const int rightCount = std::max (0, hi - std::max (ch, lo - 1));

        const float total = sumRange (lo, cl - 1) + sumRange (ch + 1, hi);
        const int   count = leftCount + rightCount;
        floorDb_[(size_t) k] = count > 0 ? total / (float) count : -120.0f;
    }

    // ---- significant peaks (used for candidate search and the harmonic check) ----
    struct Peak
    {
        float freq    = 0.0f;
        float levelDb = -120.0f;
    };

    std::array<Peak, 96> peaks {};
    int numPeaks = 0;

    for (int k = minBin; k <= maxBin && numPeaks < (int) peaks.size(); ++k)
    {
        const float level = db_[(size_t) k];

        if (level < -85.0f || level <= db_[(size_t) k - 1] || level < db_[(size_t) k + 1])
            continue;

        if (level - floorDb_[(size_t) k] < 6.0f)
            continue;

        peaks[(size_t) numPeaks++] = { (float) ((double) k * binHz), level };
    }

    // A feedback howl is a single resonance. A played note almost always brings
    // its harmonic series along, so peaks with relatives at f/3..3f are music.
    auto looksHarmonic = [&] (float freq, float level) -> bool
    {
        int matches = 0;

        for (int i = 0; i < numPeaks; ++i)
        {
            const auto& p = peaks[(size_t) i];
            const float ratio = p.freq / freq;

            const bool related = std::abs (ratio - 2.0f) < 0.03f
                              || std::abs (ratio - 3.0f) < 0.045f
                              || std::abs (ratio - 0.5f) < 0.02f
                              || std::abs (ratio - 1.0f / 3.0f) < 0.015f;

            if (related && p.levelDb > level - 18.0f)
                ++matches;
        }

        // one coincidental relation is not enough: real notes bring the series
        return matches >= 2;
    };

    // ---- peak picking (with the optional AI band mask as extra evidence) ----
    const bool aiOn = params_.aiAssist && maskFresh_;
    int numCandidates = 0;

    for (int k = minBin; k <= maxBin && numCandidates < (int) candidates_.size(); ++k)
    {
        const float level = db_[(size_t) k];

        if (level < -60.0f) // absolute gate: never chase noise
            continue;

        if (level <= db_[(size_t) k - 1] || level < db_[(size_t) k + 1])
            continue;

        float prominence = level - floorDb_[(size_t) k];
        float aiConfidence = 0.0f;

        if (aiOn)
        {
            const int band = getBandForFrequency ((float) ((double) k * binHz));
            aiConfidence = std::clamp (latestMask_.confidence[band], 0.0f, 1.0f);

            if (aiConfidence > ai::kAiBoostFloor)
                prominence += ai::kAiBoostDb * (aiConfidence - ai::kAiBoostFloor)
                                              / (1.0f - ai::kAiBoostFloor);
        }

        if (prominence < params_.sensitivityDb)
            continue;

        // a howl is narrow: neighbours four bins away must already be far down
        if (level - db_[(size_t) k - 4] < 5.0f || level - db_[(size_t) k + 4] < 5.0f)
            continue;

        if (looksHarmonic ((float) ((double) k * binHz), level))
            continue;

        const float a = db_[(size_t) k - 1];
        const float b = level;
        const float c = db_[(size_t) k + 1];
        const float denom = a - 2.0f * b + c;

        float delta = 0.0f;
        if (std::abs (denom) > 1.0e-5f)
            delta = std::clamp (0.5f * (a - c) / denom, -0.5f, 0.5f);

        Candidate cand;
        cand.freq         = (float) (((double) k + (double) delta) * binHz);
        cand.levelDb      = b - 0.25f * (a - c) * delta;
        cand.prominence   = cand.levelDb - floorDb_[(size_t) k];
        cand.aiConfidence = aiConfidence;

        if (numCandidates > 0
            && std::abs (candidates_[(size_t) numCandidates - 1].freq - cand.freq) < 2.0f * (float) binHz)
        {
            if (cand.levelDb > candidates_[(size_t) numCandidates - 1].levelDb)
                candidates_[(size_t) numCandidates - 1] = cand;
            continue;
        }

        candidates_[(size_t) numCandidates++] = cand;
    }

    // ---- tracking ----
    const int frame = ++frameCount_;
    const float frameRate = (float) (sampleRate_ / (double) kHop);
    const int attackFrames = std::max (2, (int) std::lround (params_.attackMs * 0.001f * frameRate));
    const int releaseFrames = std::max (2, (int) std::lround (params_.releaseMs * 0.001f * frameRate));

    bool seen[kMaxTracks] = {};

    for (int c = 0; c < numCandidates; ++c)
    {
        const auto& cand = candidates_[(size_t) c];
        int best = -1;
        float bestDist = 0.025f; // ~1/40 octave

        for (int t = 0; t < kMaxTracks; ++t)
        {
            const auto& track = tracks_[(size_t) t];
            if (! track.active)
                continue;

            const float dist = std::abs (std::log2 (cand.freq / std::max (track.freq, 1.0f)));
            if (dist < bestDist)
            {
                bestDist = dist;
                best     = t;
            }
        }

        if (best < 0)
        {
            for (int t = 0; t < kMaxTracks; ++t)
            {
                if (! tracks_[(size_t) t].active)
                {
                    auto& track = tracks_[(size_t) t];
                    track = Track {};
                    track.active       = true;
                    track.freq         = cand.freq;
                    track.levelDb      = cand.levelDb;
                    track.startLevelDb = cand.levelDb;
                    track.prominence   = cand.prominence;
                    track.evidence     = 1.0f;
                    track.lastSeen     = frame;
                    track.rise         = 0.0f;
                    track.aiConfidence = cand.aiConfidence;
                    track.histPos      = 0;
                    track.histCount    = 1;
                    for (float& h : track.levelHist)
                        h = cand.levelDb;
                    best = t;
                    break;
                }
            }
        }
        else
        {
            auto& track = tracks_[(size_t) best];
            const float rise = cand.levelDb - track.levelDb;
            track.rise += 0.3f * (rise - track.rise);
            track.freq       = 0.75f * track.freq + 0.25f * cand.freq;
            track.levelDb    = cand.levelDb;
            track.prominence = cand.prominence;
            track.evidence   = std::min (track.evidence + 1.0f, (float) attackFrames * 2.0f);
            track.lastSeen   = frame;
            track.aiConfidence += 0.35f * (cand.aiConfidence - track.aiConfidence);

            track.levelHist[(size_t) track.histPos] = cand.levelDb;
            track.histPos = (track.histPos + 1) % 32;
            track.histCount = std::min (track.histCount + 1, 32);

            if (cand.levelDb > track.startLevelDb + 6.0f)
                track.grown = true;
        }

        if (best >= 0)
            seen[best] = true;
    }

    const int maxNotches = std::clamp (params_.maxNotches, 1, (int) kMaxNotches);

    for (int t = 0; t < kMaxTracks; ++t)
    {
        auto& track = tracks_[(size_t) t];
        if (! track.active)
            continue;

        if (! seen[t])
        {
            track.evidence = std::max (0.0f, track.evidence - 1.5f);
            track.rise     = std::max (0.0f, track.rise - 0.5f);
            track.aiConfidence *= 0.85f;
        }
        // keep a track alive as long as it owns a notch
        if (track.evidence <= 0.0f && track.notch < 0)
        {
            track.active = false;
            continue;
        }

        // ---- engage ----
        if (track.notch < 0 && ! clearing_ && track.evidence >= (float) attackFrames)
        {
            // growth over the last half of the attack window: a howl keeps
            // climbing, a played note has settled by then
            const int halfWindow = std::clamp (attackFrames / 2, 2, 32);
            const float growth = track.histCount > halfWindow
                                   ? track.levelDb - track.levelHist[(size_t) ((track.histPos + 32 - halfWindow) % 32)]
                                   : 0.0f;

            const bool rising = growth > 2.0f;

            const float aboveProgramme = track.levelDb - rmsDb;
            const bool  strongPeak     = track.prominence >= params_.sensitivityDb + 15.0f;
            const bool  musical        = looksHarmonic (track.freq, track.levelDb);
            const bool  aiTrust        = aiOn && track.aiConfidence >= ai::kAiTrustThreshold;

            // A howl either keeps climbing, stands far out of the mix, or the
            // trained model is confident that this band is feedback.
            const bool trusted = ! params_.musicGuard
                              || aiTrust
                              || (! musical && (rising || (strongPeak && aboveProgramme >= -6.0f)));

            if (trusted)
                allocateNotch (t);
        }

        // ---- follow / release ----
        if (track.notch >= 0)
        {
            auto& notch = notches_[(size_t) track.notch];
            notch.levelDb = track.levelDb;

            if (seen[t])
            {
                const float diff = track.freq - notch.freq;
                if (std::abs (diff) > 2.0f)
                    notch.freq += 0.25f * diff; // slow retune, no zipper

                if (params_.autoQ && track.q > 0.0f)
                    notch.q += 0.10f * (std::clamp (track.q, 6.0f, 32.0f) - notch.q);
            }

            if (params_.mode == Mode::Dynamic
                && (frame - track.lastSeen) > releaseFrames)
            {
                notch.targetDb = 0.0f;

                if (notch.gainDb > -0.1f)
                {
                    freeNotch (track.notch);
                    track.notch = -1;
                    track.active = false;
                }
            }
        }
    }

    // reserve slots that the user's max-notch setting removed
    int used = 0;
    for (auto& n : notches_)
    {
        if (! n.active)
            continue;

        if (++used > maxNotches && n.targetDb < 0.0f)
            n.targetDb = 0.0f;
    }
}

void Engine::refineTracks() noexcept
{
    bool any = false;

    for (const auto& t : tracks_)
        if (t.active)
        {
            any = true;
            break;
        }

    if (! any)
        return;

    for (int i = 0; i < kFineSize; ++i)
    {
        const int idx = (histFinePos_ + i) % kFineSize;
        reFine_[(size_t) i] = histFine_[(size_t) idx] * windowFine_[(size_t) i];
        imFine_[(size_t) i] = 0.0f;
    }

    fftFine_.forward (reFine_.data(), imFine_.data());

    const float  norm  = 4.0f / (float) kFineSize;
    const double binHz = sampleRate_ / (double) kFineSize;
    const int    minBin = std::max (2, (int) std::ceil ((double) params_.lowFreq / binHz));
    const int    maxBin = std::min (kFineBins - 2, (int) std::floor ((double) params_.highFreq / binHz));

    for (int k = 0; k < kFineBins; ++k)
    {
        const float re = reFine_[(size_t) k];
        const float im = imFine_[(size_t) k];
        const float mag = std::sqrt (re * re + im * im) * norm;
        fineDb_[(size_t) k] = std::max (20.0f * std::log10 (mag + 1.0e-9f), -120.0f);
    }

    for (auto& track : tracks_)
    {
        if (! track.active)
            continue;

        const int centre = (int) std::lround ((double) track.freq / binHz);
        const int lo = std::max (minBin + 1, centre - 3);
        const int hi = std::min (maxBin - 1, centre + 3);

        if (hi <= lo)
            continue;

        int   peak   = lo;
        float peakDb = fineDb_[(size_t) lo];

        for (int k = lo + 1; k <= hi; ++k)
        {
            if (fineDb_[(size_t) k] > peakDb)
            {
                peakDb = fineDb_[(size_t) k];
                peak   = k;
            }
        }

        if (peakDb < -75.0f)
            continue;

        const float a = fineDb_[(size_t) peak - 1];
        const float b = peakDb;
        const float c = fineDb_[(size_t) peak + 1];
        const float denom = a - 2.0f * b + c;

        float delta = 0.0f;
        if (std::abs (denom) > 1.0e-5f)
            delta = std::clamp (0.5f * (a - c) / denom, -0.5f, 0.5f);

        const float refined = (float) (((double) peak + (double) delta) * binHz);

        // only trust the confirmation when it lands on the tracked resonance
        if (std::abs (refined - track.freq) > 45.0f)
            continue;

        track.freq += 0.5f * (refined - track.freq);

        // -6 dB width of the fine peak -> automatic notch Q
        const float half = peakDb - 6.0f;
        int l = peak;
        int r = peak;

        while (l > minBin && (peak - l) < 24 && fineDb_[(size_t) l - 1] > half)
            --l;

        while (r < maxBin && (r - peak) < 24 && fineDb_[(size_t) r + 1] > half)
            ++r;

        const float widthHz = (float) ((double) (r - l) * binHz);
        const float qEst = std::clamp (track.freq / std::max (widthHz, (float) binHz), 6.0f, 32.0f);
        track.q = track.q <= 0.0f ? qEst : 0.85f * track.q + 0.15f * qEst;
    }
}

void Engine::allocateNotch (int trackIndex) noexcept
{
    auto& track = tracks_[(size_t) trackIndex];
    const int maxNotches = std::clamp (params_.maxNotches, 1, (int) kMaxNotches);

    // never double-cut the same frequency twice
    for (const auto& n : notches_)
    {
        if (! n.active)
            continue;

        if (std::abs (std::log2 (n.freq / std::max (track.freq, 1.0f))) < 0.03f)
        {
            track.notch = (int) (&n - notches_.data());
            return;
        }
    }

    int slot     = -1;
    int active   = 0;

    for (int i = 0; i < maxNotches; ++i)
    {
        if (! notches_[(size_t) i].active)
        {
            slot = i;
            break;
        }
        ++active;
    }

    if (slot < 0 && active >= maxNotches)
    {
        // steal the quietest slot, but only if its howl has already gone away
        float quietest = 1.0e9f;

        for (int i = 0; i < maxNotches; ++i)
        {
            auto& n = notches_[(size_t) i];

            if (n.track >= 0)
            {
                const auto& other = tracks_[(size_t) n.track];
                if (other.evidence > 1.0f)
                    continue; // still an active howl here
            }

            if (n.levelDb < quietest)
            {
                quietest = n.levelDb;
                slot     = i;
            }
        }

        if (slot < 0)
            return; // all slots are busy with live howls
    }

    if (slot < 0)
        return;

    auto& notch = notches_[(size_t) slot];

    // the previous owner (if this slot was stolen) must forget it
    if (notch.active && notch.track >= 0 && notch.track != trackIndex)
        tracks_[(size_t) notch.track].notch = -1;

    // Adaptive attenuation: a peak that barely crosses the threshold gets a
    // shallower cut than a howl that dominates the programme.
    const float excess = std::clamp ((track.prominence - params_.sensitivityDb) / 18.0f, 0.0f, 1.0f);
    const float depth  = std::abs (params_.depthDb) * (0.8f + 0.2f * excess);

    notch = Notch {};
    notch.active   = true;
    notch.freq     = track.freq;
    notch.gainDb   = 0.0f;
    notch.targetDb = -depth;
    notch.levelDb  = track.levelDb;
    notch.q        = params_.autoQ
                       ? std::clamp (track.q > 0.0f ? track.q : params_.q, 6.0f, 32.0f)
                       : params_.q;
    notch.track    = trackIndex;
    notch.age      = frameCount_;
    notch.state[0].reset();
    notch.state[1].reset();

    track.notch = slot;
}

void Engine::freeNotch (int notchIndex) noexcept
{
    if (notchIndex < 0 || notchIndex >= (int) kMaxNotches)
        return;

    auto& n = notches_[(size_t) notchIndex];
    n.active = false;
    n.track  = -1;
    n.gainDb = 0.0f;
    n.targetDb = 0.0f;
    n.state[0].reset();
    n.state[1].reset();
}

void Engine::publishSnapshot() noexcept
{
    const uint32_t seq = snapshotSeq_.load (std::memory_order_relaxed);
    snapshotSeq_.store (seq + 1, std::memory_order_release); // odd: writing

    auto& snap = snapshot_[snapshotIndex_.load (std::memory_order_relaxed)];

    for (int k = 0; k < kDisplayBins; ++k)
    {
        snap.spectrum[k] = db_[(size_t) std::min (k, kNumBins - 1)];
        snap.floorDb[k]  = floorDb_[(size_t) std::min (k, kNumBins - 1)];
    }

    int count = 0;
    for (const auto& n : notches_)
    {
        if (n.active && count < kMaxNotches)
        {
            snap.notchFreq[count] = n.freq;
            snap.notchGain[count] = n.gainDb;
            snap.notchQ[count]    = n.q;
            ++count;
        }
    }

    snap.numNotches   = count;
    snap.hunting      = params_.mode == Mode::Off ? 0 : 1;
    snap.sensitivity  = params_.sensitivityDb;
    snap.maxFreq      = (float) (sampleRate_ * 0.5);
    snap.aiFresh      = (params_.aiAssist && maskFresh_) ? 1 : 0;
    snap.aiModel      = aiModel_;
    snap.aiConfidence = aiConfidence_;

    snapshotSeq_.store (seq + 2, std::memory_order_release); // even: readable
    snapshotIndex_ ^= 1;
}

int Engine::getDisplaySpectrum (float* out, int maxBins, float* floorOut) const noexcept
{
    if (out == nullptr || maxBins <= 0)
        return 0;

    const int n = std::min (maxBins, kDisplayBins);

    for (int attempt = 0; attempt < 4; ++attempt)
    {
        const uint32_t s1 = snapshotSeq_.load (std::memory_order_acquire);
        if (s1 & 1u)
            continue;

        const int idx = snapshotIndex_.load (std::memory_order_relaxed) ^ 1; // last fully written
        const auto& snap = snapshot_[idx];

        for (int k = 0; k < n; ++k)
        {
            out[k] = snap.spectrum[k];
            if (floorOut != nullptr)
                floorOut[k] = snap.floorDb[k];
        }

        const uint32_t s2 = snapshotSeq_.load (std::memory_order_acquire);
        if (s1 == s2)
            return n;
    }

    return 0;
}

int Engine::getNotchInfo (NotchInfo* out, int maxOut) const noexcept
{
    if (out == nullptr || maxOut <= 0)
        return 0;

    int result = 0;

    for (int attempt = 0; attempt < 4; ++attempt)
    {
        const uint32_t s1 = snapshotSeq_.load (std::memory_order_acquire);
        if (s1 & 1u)
            continue;

        const auto& snap = snapshot_[snapshotIndex_.load (std::memory_order_relaxed) ^ 1];
        result = std::min (snap.numNotches, maxOut);

        for (int i = 0; i < result; ++i)
        {
            out[i].freq    = snap.notchFreq[i];
            out[i].gainDb  = snap.notchGain[i];
            out[i].levelDb = -120.0f;
            out[i].q       = snap.notchQ[i];
        }

        const uint32_t s2 = snapshotSeq_.load (std::memory_order_acquire);
        if (s1 == s2)
            return result;
    }

    return 0;
}

int Engine::getNumNotches() const noexcept
{
    for (int attempt = 0; attempt < 4; ++attempt)
    {
        const uint32_t s1 = snapshotSeq_.load (std::memory_order_acquire);
        if (s1 & 1u)
            continue;

        const int n = snapshot_[snapshotIndex_.load (std::memory_order_relaxed) ^ 1].numNotches;

        if (snapshotSeq_.load (std::memory_order_acquire) == s1)
            return n;
    }

    return 0;
}

AiState Engine::getAiState() const noexcept
{
    AiState state {};
    state.patchRevision = getAiPatchRevision();

    for (int attempt = 0; attempt < 4; ++attempt)
    {
        const uint32_t s1 = snapshotSeq_.load (std::memory_order_acquire);
        if (s1 & 1u)
            continue;

        const auto& snap = snapshot_[snapshotIndex_.load (std::memory_order_relaxed) ^ 1];

        state.maskFresh  = snap.aiFresh != 0;
        state.confidence = snap.aiConfidence;
        state.model      = snap.aiModel;

        const uint32_t s2 = snapshotSeq_.load (std::memory_order_acquire);
        if (s1 == s2)
            return state;
    }

    return state;
}

} // namespace defb
