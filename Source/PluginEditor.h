#pragma once

#include <array>
#include <functional>
#include <memory>
#include <vector>

#include <juce_gui_extra/juce_gui_extra.h>

#include "PluginProcessor.h"

namespace defbui
{
    inline const juce::Colour bg     { 0xff0d1016 };
    inline const juce::Colour panel  { 0xff141922 };
    inline const juce::Colour panelHi{ 0xff1b222d };
    inline const juce::Colour stroke { 0xff28313f };
    inline const juce::Colour text   { 0xffe8eef7 };
    inline const juce::Colour muted  { 0xff7c8798 };
    inline const juce::Colour accent { 0xff36d3bd };
    inline const juce::Colour warn   { 0xffffa04a };

    inline juce::Font font (float height, bool bold = false)
    {
        juce::Font f { juce::FontOptions (height) };
        return bold ? f.boldened() : f;
    }
}

//==============================================================================
class DeFeedbackLookAndFeel : public juce::LookAndFeel_V4
{
public:
    DeFeedbackLookAndFeel();

    void drawRotarySlider (juce::Graphics&, int x, int y, int width, int height,
                           float sliderPosProportional, float rotaryStartAngle,
                           float rotaryEndAngle, juce::Slider&) override;

    void drawButtonBackground (juce::Graphics&, juce::Button&, const juce::Colour& backgroundColour,
                               bool shouldDrawButtonAsHighlighted,
                               bool shouldDrawButtonAsDown) override;

    void drawButtonText (juce::Graphics&, juce::TextButton&,
                         bool shouldDrawButtonAsHighlighted,
                         bool shouldDrawButtonAsDown) override;
};

//==============================================================================
class SpectrumDisplay : public juce::Component
{
public:
    explicit SpectrumDisplay (DeFeedbackAudioProcessor&);

    void paint (juce::Graphics&) override;
    void refresh();

    static float xForFreq (float freq, juce::Rectangle<float> area);
    static float yForDb (float db, juce::Rectangle<float> area);

private:
    DeFeedbackAudioProcessor& processor;

    std::array<float, defb::Engine::kDisplayBins> smoothed {};
    bool primed = false;
};

//==============================================================================
class DeFeedbackAudioProcessorEditor : public juce::AudioProcessorEditor,
                                       private juce::Timer
{
public:
    explicit DeFeedbackAudioProcessorEditor (DeFeedbackAudioProcessor&);
    ~DeFeedbackAudioProcessorEditor() override;

    void paint (juce::Graphics&) override;
    void resized() override;

private:
    struct Knob
    {
        juce::Slider slider;
        juce::Label label;
        std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> attachment;
    };

    void timerCallback() override;
    void addKnob (const juce::String& paramID, const juce::String& name,
                  std::function<juce::String (double)> format);
    void updateModeButtons();
    void updateStatus();

    DeFeedbackAudioProcessor& processor;

    DeFeedbackLookAndFeel lookAndFeel;
    SpectrumDisplay spectrum;

    std::vector<std::unique_ptr<Knob>> knobs;
    juce::TextButton modeButtons[3];
    juce::TextButton guardButton { "MUSIC GUARD" };
    std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment> guardAttachment;
    juce::TextButton aiButton { "AI" };
    std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment> aiAttachment;
    juce::TextButton autoQButton { "AUTO Q" };
    std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment> autoQAttachment;
    juce::TextButton clearButton { "CLEAR" };
    juce::Label statusLabel;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (DeFeedbackAudioProcessorEditor)
};
