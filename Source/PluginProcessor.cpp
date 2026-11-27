#include "PluginProcessor.h"
#include "PluginEditor.h"

#include <cmath>
#include <cstdlib>
#include <memory>
#include <vector>

//==============================================================================
// Runs the band-mask model outside the audio callback. The audio thread only
// publishes the 64 x 256 patch and pops finished masks from a lock-free queue;
// it never waits for this thread.
class AiWorkerThread final : public juce::Thread
{
public:
    explicit AiWorkerThread (DeFeedbackAudioProcessor& p)
        : juce::Thread ("ResonanceAI worker"), processor (p)
    {
        patch.assign ((size_t) defb::ai::kPatchSize, 0.0f);
        mask.assign ((size_t) defb::ai::kNumBands, 0.0f);
    }

    void run() override
    {
        uint32_t lastRevision = 0;

        while (! threadShouldExit())
        {
            auto& engine = processor.engine;
            const uint32_t revision = engine.getAiPatchRevision();

            if (revision != lastRevision
                && processor.maskModel != nullptr
                && processor.maskModel->ready())
            {
                lastRevision = revision;

                if (engine.copyAiPatch (patch.data(), (int) patch.size()) == (int) patch.size())
                {
                    float clip = 0.5f;
                    const double start = juce::Time::getMillisecondCounterHiRes();
                    const bool ok = processor.maskModel->infer (patch.data(), mask.data(), clip);
                    const float ms = (float) (juce::Time::getMillisecondCounterHiRes() - start);

                    processor.aiInferenceMs.store (ms);

                    if (ok)
                    {
                        defb::ai::Mask message {};

                        for (int b = 0; b < defb::ai::kNumBands; ++b)
                            message.confidence[b] = mask[(size_t) b];

                        message.clipProbability = clip;
                        message.model           = processor.maskModel->kind();
                        message.timestampMs     = defb::ai::nowMs();

                        engine.postAiMask (message);
                    }
                }
            }

            wait (15);
        }
    }

private:
    DeFeedbackAudioProcessor& processor;
    std::vector<float> patch;
    std::vector<float> mask;
};

//==============================================================================
namespace
{
    juce::NormalisableRange<float> skewedRange (float lo, float hi, float centre)
    {
        juce::NormalisableRange<float> range (lo, hi);
        range.setSkewForCentre (centre);
        return range;
    }

    juce::String computeUnitsName (defb::ai::ComputeUnits units)
    {
        switch (units)
        {
            case defb::ai::ComputeUnits::cpuOnly:            return "CPU only";
            case defb::ai::ComputeUnits::all:                return "All units";
            case defb::ai::ComputeUnits::cpuAndNeuralEngine:
            default:                                         return "CPU + Neural Engine";
        }
    }

    defb::ai::ComputeUnits computeUnitsFromEnvironment()
    {
        if (const char* value = std::getenv ("RESONANCE_AI_COMPUTE_UNITS"))
        {
            const int choice = juce::String (value).getIntValue();

            if (choice == 1) return defb::ai::ComputeUnits::cpuOnly;
            if (choice == 3) return defb::ai::ComputeUnits::all;
        }

        return defb::ai::ComputeUnits::cpuAndNeuralEngine;
    }
}

//==============================================================================
juce::AudioProcessorValueTreeState::ParameterLayout DeFeedbackAudioProcessor::createLayout()
{
    using namespace juce;

    std::vector<std::unique_ptr<RangedAudioParameter>> params;

    params.push_back (std::make_unique<AudioParameterChoice> (
        ParameterID { "mode", 1 }, "Mode", StringArray { "Off", "Hunt", "Dynamic" }, 1));

    params.push_back (std::make_unique<AudioParameterFloat> (
        ParameterID { "sensitivity", 1 }, "Sensitivity",
        NormalisableRange<float> (3.0f, 24.0f, 0.1f), 9.0f,
        AudioParameterFloatAttributes().withLabel ("dB")));

    params.push_back (std::make_unique<AudioParameterInt> (
        ParameterID { "maxNotches", 1 }, "Max Notches", 1, defb::Engine::kMaxNotches, 8));

    params.push_back (std::make_unique<AudioParameterFloat> (
        ParameterID { "depth", 1 }, "Notch Depth",
        NormalisableRange<float> (3.0f, 36.0f, 1.0f), 18.0f,
        AudioParameterFloatAttributes().withLabel ("dB")));

    params.push_back (std::make_unique<AudioParameterFloat> (
        ParameterID { "width", 1 }, "Notch Width",
        NormalisableRange<float> (2.0f, 48.0f, 0.5f), 16.0f,
        AudioParameterFloatAttributes().withLabel ("Q")));

    params.push_back (std::make_unique<AudioParameterFloat> (
        ParameterID { "attack", 1 }, "Attack",
        skewedRange (10.0f, 2000.0f, 120.0f), 60.0f,
        AudioParameterFloatAttributes().withLabel ("ms")));

    params.push_back (std::make_unique<AudioParameterFloat> (
        ParameterID { "release", 1 }, "Release",
        skewedRange (100.0f, 20000.0f, 2000.0f), 2500.0f,
        AudioParameterFloatAttributes().withLabel ("ms")));

    params.push_back (std::make_unique<AudioParameterFloat> (
        ParameterID { "lowFreq", 1 }, "Low Limit",
        skewedRange (30.0f, 800.0f, 120.0f), 40.0f,
        AudioParameterFloatAttributes().withLabel ("Hz")));

    params.push_back (std::make_unique<AudioParameterFloat> (
        ParameterID { "highFreq", 1 }, "High Limit",
        skewedRange (2000.0f, 20000.0f, 8000.0f), 16000.0f,
        AudioParameterFloatAttributes().withLabel ("Hz")));

    params.push_back (std::make_unique<AudioParameterBool> (
        ParameterID { "musicGuard", 1 }, "Music Guard", true));

    params.push_back (std::make_unique<AudioParameterBool> (
        ParameterID { "aiAssist", 1 }, "AI Assist", true));

    params.push_back (std::make_unique<AudioParameterBool> (
        ParameterID { "autoQ", 1 }, "Auto Q", true));

    return { params.begin(), params.end() };
}

//==============================================================================
DeFeedbackAudioProcessor::DeFeedbackAudioProcessor()
    : AudioProcessor (BusesProperties()
                          .withInput ("Input", juce::AudioChannelSet::stereo(), true)
                          .withOutput ("Output", juce::AudioChannelSet::stereo(), true)),
      apvts (*this, nullptr, "PARAMETERS", createLayout())
{
    modeParam        = apvts.getRawParameterValue ("mode");
    sensitivityParam = apvts.getRawParameterValue ("sensitivity");
    maxNotchesParam  = apvts.getRawParameterValue ("maxNotches");
    depthParam       = apvts.getRawParameterValue ("depth");
    qParam           = apvts.getRawParameterValue ("width");
    attackParam      = apvts.getRawParameterValue ("attack");
    releaseParam     = apvts.getRawParameterValue ("release");
    lowParam         = apvts.getRawParameterValue ("lowFreq");
    highParam        = apvts.getRawParameterValue ("highFreq");
    guardParam       = apvts.getRawParameterValue ("musicGuard");
    aiAssistParam    = apvts.getRawParameterValue ("aiAssist");
    autoQParam       = apvts.getRawParameterValue ("autoQ");

    // ---- ResonanceAI model selection -------------------------------------
    const auto units = computeUnitsFromEnvironment();

    if (auto core = defb::ai::createCoreMlMaskModel (units))
    {
        juce::StringArray candidates;

        const auto envPath = juce::SystemStats::getEnvironmentVariable ("RESONANCE_AI_MODEL", {});

        if (envPath.isNotEmpty())
            candidates.add (envPath);

        candidates.add (juce::File::getSpecialLocation (juce::File::userApplicationDataDirectory)
                            .getChildFile ("ResonanceAI")
                            .getChildFile ("ResonanceFeedback.mlmodelc")
                            .getFullPathName());

        candidates.add (juce::String()); // empty path: look inside the plugin bundle

        for (const auto& path : candidates)
        {
            if (core->load (path.toStdString()))
            {
                maskModel = std::move (core);
                break;
            }
        }
    }

    if (maskModel == nullptr)
        maskModel = defb::ai::createBuiltinMaskModel();

    aiModelName  = maskModel->name();
    aiModelKind.store (maskModel->kind());
    aiModelReady.store (maskModel->ready());
    aiComputeName = maskModel->kind() == defb::ai::kModelCoreMl
                      ? computeUnitsName (units)
                      : juce::String ("DSP fallback");

    aiWorker = std::make_unique<AiWorkerThread> (*this);
    aiWorker->startThread (juce::Thread::Priority::low);
}

DeFeedbackAudioProcessor::~DeFeedbackAudioProcessor()
{
    if (aiWorker != nullptr)
    {
        aiWorker->signalThreadShouldExit();
        aiWorker->stopThread (2000);
        aiWorker.reset();
    }
}

void DeFeedbackAudioProcessor::prepareToPlay (double sampleRate, int samplesPerBlock)
{
    engine.prepare (sampleRate, samplesPerBlock);
    aiInferenceMs.store (0.0f);
}

bool DeFeedbackAudioProcessor::isBusesLayoutSupported (const BusesLayout& layouts) const
{
    const auto& mainIn  = layouts.getMainInputChannelSet();
    const auto& mainOut = layouts.getMainOutputChannelSet();

    if (mainIn != mainOut)
        return false;

    return mainOut == juce::AudioChannelSet::mono()
        || mainOut == juce::AudioChannelSet::stereo();
}

void DeFeedbackAudioProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer&)
{
    juce::ScopedNoDenormals noDenormals;

    const int numIn  = getTotalNumInputChannels();
    const int numOut = getTotalNumOutputChannels();
    const int numSamples = buffer.getNumSamples();

    for (int ch = numIn; ch < numOut; ++ch)
        buffer.clear (ch, 0, numSamples);

    if (numIn == 0 || numSamples == 0)
        return;

    defb::Parameters p;
    p.mode          = static_cast<defb::Mode> ((int) std::lround (modeParam->load()));
    p.sensitivityDb = sensitivityParam->load();
    p.maxNotches    = (int) std::lround (maxNotchesParam->load());
    p.depthDb       = depthParam->load();
    p.q             = qParam->load();
    p.attackMs      = attackParam->load();
    p.releaseMs     = releaseParam->load();
    p.lowFreq       = lowParam->load();
    p.highFreq      = highParam->load();
    p.musicGuard    = guardParam->load() > 0.5f;
    p.aiAssist      = aiAssistParam->load() > 0.5f;
    p.autoQ         = autoQParam->load() > 0.5f;

    engine.setParameters (p);

    if (numIn >= 2)
        engine.process (buffer.getWritePointer (0), buffer.getWritePointer (1), numSamples);
    else
        engine.process (buffer.getWritePointer (0), nullptr, numSamples);
}

DeFeedbackAudioProcessor::AiRuntimeInfo DeFeedbackAudioProcessor::getAiRuntimeInfo() const
{
    AiRuntimeInfo info;

    info.modelName    = aiModelName;
    info.computeUnits = aiComputeName;
    info.modelLoaded  = aiModelReady.load();
    info.modelKind    = aiModelKind.load();
    info.inferenceMs  = aiInferenceMs.load();

    const auto state = engine.getAiState();
    info.maskFresh  = state.maskFresh;
    info.confidence = state.confidence;

    return info;
}

void DeFeedbackAudioProcessor::clearNotches()
{
    engine.clearNotches();
}

juce::AudioProcessorEditor* DeFeedbackAudioProcessor::createEditor()
{
    return new DeFeedbackAudioProcessorEditor (*this);
}

void DeFeedbackAudioProcessor::getStateInformation (juce::MemoryBlock& destData)
{
    if (auto xml = apvts.copyState().createXml())
        copyXmlToBinary (*xml, destData);
}

void DeFeedbackAudioProcessor::setStateInformation (const void* data, int sizeInBytes)
{
    if (auto xml = getXmlFromBinary (data, sizeInBytes))
    {
        if (xml->hasTagName (apvts.state.getType()))
            apvts.replaceState (juce::ValueTree::fromXml (*xml));

        engine.clearNotches();
    }
}

//==============================================================================
juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new DeFeedbackAudioProcessor();
}
