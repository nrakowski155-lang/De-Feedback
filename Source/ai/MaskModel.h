#pragma once

#include <memory>
#include <string>

#include "AiTypes.h"

#ifndef DEFB_WITH_COREML
 #define DEFB_WITH_COREML 0
#endif

namespace defb::ai
{

enum class ComputeUnits
{
    cpuOnly            = 1,
    cpuAndNeuralEngine = 2,
    all                = 3
};

// A model turns one 64 x 256 spectrogram patch into a per-band feedback
// probability. Implementations must be safe to call from the AI worker thread
// only; they never run inside the audio callback.
class MaskModel
{
public:
    virtual ~MaskModel() = default;

    virtual const char* name() const noexcept = 0;
    virtual int  kind() const noexcept = 0;
    virtual bool ready() const noexcept = 0;

    // Loads a model bundle/resource; the built-in baseline ignores the path.
    virtual bool load (const std::string& resourcePath) noexcept = 0;

    // patch: [band][frame], band-major, newest frame last, normalised dB 0..1
    virtual bool infer (const float* patch, float* bandMask, float& clipProbability) noexcept = 0;
};

// Band-wise narrowness/growth baseline. Always available, used as the fallback
// when no trained Core ML model is installed.
std::unique_ptr<MaskModel> createBuiltinMaskModel();

// Returns nullptr when the platform has no Core ML support or the bridge is
// compiled out. The returned model still has to be loaded (loadFromPath).
std::unique_ptr<MaskModel> createCoreMlMaskModel (ComputeUnits units);

} // namespace defb::ai
