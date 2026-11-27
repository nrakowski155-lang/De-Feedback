#include "MaskModel.h"

#include <algorithm>
#include <cmath>

namespace defb::ai
{

namespace
{

constexpr int kFrames = kPatchFrames;
constexpr int kBands  = kNumBands;

inline float at (const float* patch, int band, int frame) noexcept
{
    return patch[(size_t) band * (size_t) kFrames + (size_t) frame];
}

// patch values are normalised dB: 0 = -100 dBFS, 1 = 0 dBFS
inline float toDb (float value) noexcept
{
    return value * 100.0f - 100.0f;
}

// ---------------------------------------------------------------------------
// Built-in baseline ("ResonanceAI Builtin"): a narrow band that stands far
// above its neighbours is a resonance candidate; growth raises the confidence.
// It is intentionally conservative - it only *adds* evidence to the DSP
// tracker and is replaced by the trained Core ML model when one is installed.
// ---------------------------------------------------------------------------
class BuiltinMaskModel final : public MaskModel
{
public:
    const char* name() const noexcept override { return "Builtin"; }
    int  kind() const noexcept override { return kModelBuiltin; }
    bool ready() const noexcept override { return true; }
    bool load (const std::string&) noexcept override { return true; }

    bool infer (const float* patch, float* bandMask, float& clipProbability) noexcept override
    {
        if (patch == nullptr || bandMask == nullptr)
            return false;

        float peak = 0.0f;

        for (int b = 0; b < kBands; ++b)
        {
            const float levelDb = toDb (at (patch, b, kFrames - 1));

            float neighbourDb = 0.0f;
            int   neighbours  = 0;

            for (int d = 2; d <= 4; ++d)
            {
                if (b - d >= 0)
                {
                    neighbourDb += toDb (at (patch, b - d, kFrames - 1));
                    ++neighbours;
                }

                if (b + d < kBands)
                {
                    neighbourDb += toDb (at (patch, b + d, kFrames - 1));
                    ++neighbours;
                }
            }

            const float contrastDb = neighbours > 0
                                       ? levelDb - neighbourDb / (float) neighbours
                                       : 0.0f;

            // ~85 ms trend (16 hops at 5.33 ms/hop)
            const float recentDb = 0.5f * (toDb (at (patch, b, kFrames - 1))
                                         + toDb (at (patch, b, kFrames - 9)));
            const float olderDb  = 0.5f * (toDb (at (patch, b, kFrames - 17))
                                         + toDb (at (patch, b, kFrames - 25)));
            const float growthDb = recentDb - olderDb;

            const float narrow = std::clamp (contrastDb / 12.0f, 0.0f, 1.0f);
            const float growth = std::clamp ((growthDb + 3.0f) / 9.0f, 0.0f, 1.0f);

            const float confidence = std::clamp (0.10f + 0.90f * narrow * (0.65f + 0.35f * growth),
                                                 0.0f, 1.0f);

            bandMask[b] = confidence;
            peak = std::max (peak, confidence);
        }

        clipProbability = peak;
        return true;
    }
};

} // namespace

std::unique_ptr<MaskModel> createBuiltinMaskModel()
{
    return std::make_unique<BuiltinMaskModel>();
}

#if ! DEFB_WITH_COREML
std::unique_ptr<MaskModel> createCoreMlMaskModel (ComputeUnits)
{
    return nullptr;
}
#endif

} // namespace defb::ai
