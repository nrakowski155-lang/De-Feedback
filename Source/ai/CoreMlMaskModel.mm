// Core ML bridge for ResonanceAI.
//
// This file is only compiled on Apple platforms (see CMakeLists.txt), where
// DEFB_WITH_COREML=1. It loads a compiled .mlmodelc with exactly the tensor
// contract documented in AiTypes.h and runs one synchronous prediction per
// call. The plugin only ever calls this from its AI worker thread - never from
// the real-time audio callback.
//
// Model location (first hit wins, tried by the processor):
//   1. $RESONANCE_AI_MODEL
//   2. ~/Library/Application Support/ResonanceAI/ResonanceFeedback.mlmodelc
//   3. <plugin bundle>/Contents/Resources/ResonanceFeedback.mlmodelc

#include "MaskModel.h"

#if DEFB_WITH_COREML

#import <CoreML/CoreML.h>
#import <Foundation/Foundation.h>

#include <algorithm>
#include <cstring>
#include <string>

#if __has_feature (objc_arc)
 #define DEFB_RELEASE(x) ((void) 0)
#else
 #define DEFB_RELEASE(x) [(x) release]
#endif

namespace defb::ai
{

namespace
{
    NSString* toNSString (const std::string& s)
    {
        return [NSString stringWithUTF8String: s.c_str()];
    }
}

class CoreMlMaskModel final : public MaskModel
{
public:
    explicit CoreMlMaskModel (ComputeUnits units) : units_ (units) {}
    ~CoreMlMaskModel() override
    {
        DEFB_RELEASE (model_);
        model_ = nil;
    }

    const char* name() const noexcept override { return "Core ML"; }
    int  kind() const noexcept override { return kModelCoreMl; }
    bool ready() const noexcept override { return model_ != nil; }

    bool load (const std::string& resourcePath) noexcept override
    {
        @autoreleasepool
        {
            if (resourcePath.empty())
            {
                // The host loads the AU bundle into its own process, so the
                // plugin bundle is looked up by its bundle identifier.
                NSBundle* bundle = [NSBundle bundleWithIdentifier: @"com.defeedback.plugin"];
                NSString* bundled = bundle != nil
                                      ? [bundle pathForResource: @"ResonanceFeedback" ofType: @"mlmodelc"]
                                      : nil;

                if (bundled == nil)
                    return false;

                return load (std::string ([bundled UTF8String]));
            }

            NSURL* url = [NSURL fileURLWithPath: toNSString (resourcePath) isDirectory: YES];

            if (url == nil)
                return false;

            MLModelConfiguration* config = [[MLModelConfiguration alloc] init];

            if (@available (macOS 12.0, *))
            {
                switch (units_)
                {
                    case ComputeUnits::cpuOnly:            config.computeUnits = MLComputeUnitsCPUOnly; break;
                    case ComputeUnits::all:                config.computeUnits = MLComputeUnitsAll; break;
                    case ComputeUnits::cpuAndNeuralEngine:
                    default:                               config.computeUnits = MLComputeUnitsCPUAndNeuralEngine; break;
                }
            }

            NSError* error = nil;
            MLModel* model = [[MLModel alloc] initWithContentsOfURL: url
                                                      configuration: config
                                                              error: &error];
            DEFB_RELEASE (config);

            if (model == nil)
            {
                NSLog (@"ResonanceAI: Core ML model could not be loaded from %@: %@", url, error);
                return false;
            }

            model_ = model; // owned by the model object under MRC, strong under ARC

            float warmPatch[kPatchSize] {};
            float warmMask[kNumBands] {};
            float clip = 0.0f;

            if (! infer (warmPatch, warmMask, clip))
            {
                NSLog (@"ResonanceAI: Core ML warm-up failed for %@", url);
                DEFB_RELEASE (model_);
                model_ = nil;
                return false;
            }

            return true;
        }
    }

    bool infer (const float* patch, float* bandMask, float& clipProbability) noexcept override
    {
        if (model_ == nil || patch == nullptr || bandMask == nullptr)
            return false;

        @autoreleasepool
        {
            NSError* error = nil;

            MLMultiArray* input = [[MLMultiArray alloc] initWithShape: @[ @1, @1,
                                                                         @(kNumBands),
                                                                         @(kPatchFrames) ]
                                                            dataType: MLMultiArrayDataTypeFloat32
                                                               error: &error];
            if (input == nil)
            {
                NSLog (@"ResonanceAI: could not allocate the model input: %@", error);
                return false;
            }

            std::memcpy (input.dataPointer, patch, sizeof (float) * (size_t) kPatchSize);

            MLDictionaryFeatureProvider* provider =
                [[MLDictionaryFeatureProvider alloc] initWithDictionary: @{ @"spectrogram": input }
                                                                  error: &error];
            DEFB_RELEASE (input);

            if (provider == nil)
            {
                NSLog (@"ResonanceAI: could not build model features: %@", error);
                return false;
            }

            id<MLFeatureProvider> output = [model_ predictionFromFeatures: provider error: &error];
            DEFB_RELEASE (provider);

            if (output == nil)
            {
                NSLog (@"ResonanceAI: inference failed: %@", error);
                return false;
            }

            MLMultiArray* mask = [[output featureValueForName: @"mask"] multiArrayValue];

            if (mask == nil || mask.count < (NSInteger) kNumBands)
            {
                NSLog (@"ResonanceAI: model has no usable \"mask\" output");
                return false;
            }

            const float* src = (const float*) mask.dataPointer;

            for (int b = 0; b < kNumBands; ++b)
                bandMask[b] = std::clamp (src[b], 0.0f, 1.0f);

            MLFeatureValue* clip = [output featureValueForName: @"clip"];
            MLMultiArray* clipArray = clip != nil ? clip.multiArrayValue : nil;

            clipProbability = clipArray != nil && clipArray.count > 0
                                ? (float) [clipArray objectAtIndexedSubscript: 0].doubleValue
                                : 0.5f;
            clipProbability = std::clamp (clipProbability, 0.0f, 1.0f);

            return true;
        }
    }

private:
    ComputeUnits units_ = ComputeUnits::cpuAndNeuralEngine;
    MLModel* model_ = nil;
};

std::unique_ptr<MaskModel> createCoreMlMaskModel (ComputeUnits units)
{
    return std::make_unique<CoreMlMaskModel> (units);
}

} // namespace defb::ai

#endif // DEFB_WITH_COREML
