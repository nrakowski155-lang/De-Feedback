#pragma once

#include <atomic>

#include <juce_audio_processors/juce_audio_processors.h>

#include "dsp/DeFeedbackEngine.h"

class DeFeedbackAudioProcessor : public juce::AudioProcessor
{
public:
    DeFeedbackAudioProcessor();
    ~DeFeedbackAudioProcessor() override = default;

    void prepareToPlay (double sampleRate, int samplesPerBlock) override;
    void releaseResources() override {}
    bool isBusesLayoutSupported (const BusesLayout& layouts) const override;
    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override { return true; }

    const juce::String getName() const override { return JucePlugin_Name; }
    bool acceptsMidi() const override { return false; }
    bool producesMidi() const override { return false; }
    bool isMidiEffect() const override { return false; }
    double getTailLengthSeconds() const override { return 0.0; }

    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram (int) override {}
    const juce::String getProgramName (int) override { return {}; }
    void changeProgramName (int, const juce::String&) override {}

    void getStateInformation (juce::MemoryBlock& destData) override;
    void setStateInformation (const void* data, int sizeInBytes) override;

    juce::AudioProcessorValueTreeState& getApvts() noexcept { return apvts; }
    defb::Engine& getEngine() noexcept { return engine; }

    // Removes every learned notch (fade out, no click).
    void clearNotches();

    static juce::AudioProcessorValueTreeState::ParameterLayout createLayout();

private:
    defb::Engine engine;

    juce::AudioProcessorValueTreeState apvts;

    std::atomic<float>* modeParam       = nullptr;
    std::atomic<float>* sensitivityParam = nullptr;
    std::atomic<float>* maxNotchesParam = nullptr;
    std::atomic<float>* depthParam      = nullptr;
    std::atomic<float>* qParam          = nullptr;
    std::atomic<float>* attackParam     = nullptr;
    std::atomic<float>* releaseParam    = nullptr;
    std::atomic<float>* lowParam        = nullptr;
    std::atomic<float>* highParam       = nullptr;
    std::atomic<float>* guardParam      = nullptr;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (DeFeedbackAudioProcessor)
};
