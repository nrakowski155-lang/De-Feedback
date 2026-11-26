#include "PluginProcessor.h"
#include "PluginEditor.h"

#include <cmath>
#include <memory>
#include <vector>

//==============================================================================
namespace
{
    juce::NormalisableRange<float> skewedRange (float lo, float hi, float centre)
    {
        juce::NormalisableRange<float> range (lo, hi);
        range.setSkewForCentre (centre);
        return range;
    }
}

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
        ParameterID { "maxNotches", 1 }, "Max Notches", 1, 16, 8));

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
        skewedRange (10.0f, 2000.0f, 300.0f), 250.0f,
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
}

void DeFeedbackAudioProcessor::prepareToPlay (double sampleRate, int samplesPerBlock)
{
    engine.prepare (sampleRate, samplesPerBlock);
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

    engine.setParameters (p);

    if (numIn >= 2)
        engine.process (buffer.getWritePointer (0), buffer.getWritePointer (1), numSamples);
    else
        engine.process (buffer.getWritePointer (0), nullptr, numSamples);
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
