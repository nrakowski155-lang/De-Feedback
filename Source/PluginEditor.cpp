#include "PluginEditor.h"

#include <cmath>

using namespace defbui;

//==============================================================================
DeFeedbackLookAndFeel::DeFeedbackLookAndFeel()
{
    setColourScheme ({ bg, panel, bg, stroke, text, panel, bg, accent, text });

    setColour (juce::TextButton::buttonColourId,   panel);
    setColour (juce::TextButton::buttonOnColourId, accent);
    setColour (juce::TextButton::textColourOffId,  muted);
    setColour (juce::TextButton::textColourOnId,   bg);
    setColour (juce::ToggleButton::textColourId,   muted);
    setColour (juce::ToggleButton::tickColourId,   accent);
}

void DeFeedbackLookAndFeel::drawRotarySlider (juce::Graphics& g, int x, int y, int width, int height,
                                              float sliderPos, float startAngle, float endAngle,
                                              juce::Slider& slider)
{
    auto bounds = juce::Rectangle<float> ((float) x, (float) y, (float) width, (float) height).reduced (3.0f);
    const auto centre = bounds.getCentre();
    const float radius = juce::jmin (bounds.getWidth(), bounds.getHeight()) * 0.5f;
    const float lineW  = juce::jmax (2.0f, radius * 0.085f);
    const float angle  = startAngle + sliderPos * (endAngle - startAngle);

    // recessed body
    g.setColour (panel);
    g.fillEllipse (juce::Rectangle<float> (radius * 1.72f, radius * 1.72f).withCentre (centre));
    g.setColour (stroke.withAlpha (0.7f));
    g.drawEllipse (juce::Rectangle<float> (radius * 1.72f, radius * 1.72f).withCentre (centre), 1.0f);

    // track
    juce::Path track;
    track.addCentredArc (centre.x, centre.y, radius - lineW, radius - lineW,
                         0.0f, startAngle, endAngle, true);
    g.setColour (stroke);
    g.strokePath (track, juce::PathStrokeType (lineW, juce::PathStrokeType::curved,
                                               juce::PathStrokeType::rounded));

    // value arc
    if (sliderPos > 0.001f)
    {
        juce::Path arc;
        arc.addCentredArc (centre.x, centre.y, radius - lineW, radius - lineW,
                           0.0f, startAngle, angle, true);
        g.setColour (accent.withAlpha (slider.isEnabled() ? 0.95f : 0.35f));
        g.strokePath (arc, juce::PathStrokeType (lineW, juce::PathStrokeType::curved,
                                                 juce::PathStrokeType::rounded));
    }

    // pointer
    juce::Path pointer;
    pointer.startNewSubPath (0.0f, -(radius * 0.30f));
    pointer.lineTo (0.0f, -(radius - lineW * 2.0f));
    g.setColour (text);
    g.strokePath (pointer, juce::PathStrokeType (juce::jmax (1.6f, lineW * 0.55f),
                                                 juce::PathStrokeType::curved,
                                                 juce::PathStrokeType::rounded),
                  juce::AffineTransform::rotation (angle).translated (centre));

    // value
    g.setFont (font (juce::jmax (9.0f, radius * 0.30f), false));
    g.setColour (text.withAlpha (0.9f));
    g.drawText (slider.getTextFromValue (slider.getValue()),
                bounds.withTrimmedTop (radius * 0.55f).translated (0.0f, -radius * 0.12f),
                juce::Justification::centred, false);
}

void DeFeedbackLookAndFeel::drawButtonBackground (juce::Graphics& g, juce::Button& button,
                                                  const juce::Colour&, bool highlighted, bool down)
{
    auto bounds = button.getLocalBounds().toFloat().reduced (0.5f);
    const bool on = button.getToggleState();

    auto fill = on ? accent : panel.brighter (0.05f);
    if (down)          fill = fill.brighter (0.10f);
    else if (highlighted) fill = fill.brighter (0.05f);

    g.setColour (fill);
    g.fillRoundedRectangle (bounds, 5.0f);
    g.setColour (on ? accent.brighter (0.25f) : stroke);
    g.drawRoundedRectangle (bounds, 5.0f, 1.0f);
}

void DeFeedbackLookAndFeel::drawButtonText (juce::Graphics& g, juce::TextButton& button,
                                            bool, bool)
{
    g.setFont (font (10.0f, true));
    g.setColour (button.getToggleState() ? bg : muted.brighter (0.45f));
    g.drawText (button.getButtonText(), button.getLocalBounds(),
                juce::Justification::centred, false);
}

//==============================================================================
SpectrumDisplay::SpectrumDisplay (DeFeedbackAudioProcessor& p) : processor (p)
{
    smoothed.fill (-96.0f);
    setInterceptsMouseClicks (false, false);
}

float SpectrumDisplay::xForFreq (float freq, juce::Rectangle<float> area)
{
    const float t = std::log (juce::jlimit (20.0f, 20000.0f, freq) / 20.0f) / std::log (1000.0f);
    return area.getX() + juce::jlimit (0.0f, 1.0f, t) * area.getWidth();
}

float SpectrumDisplay::yForDb (float db, juce::Rectangle<float> area)
{
    const float t = juce::jlimit (0.0f, 1.0f, (db + 84.0f) / 84.0f);
    return area.getBottom() - t * area.getHeight();
}

void SpectrumDisplay::refresh()
{
    if (! processor.getEngine().isAnalyzing())
    {
        for (auto& value : smoothed)
            value += (-100.0f - value) * 0.02f;

        repaint();
        return;
    }

    std::array<float, defb::Engine::kDisplayBins> raw {};
    std::array<float, defb::Engine::kDisplayBins> floor {};
    const int n = processor.getEngine().getDisplaySpectrum (raw.data(), (int) raw.size(), floor.data());

    if (n > 0)
    {
        for (int i = 0; i < n; ++i)
        {
            const float target = std::isfinite (raw[(size_t) i])
                                   ? juce::jlimit (-100.0f, 6.0f, raw[(size_t) i])
                                   : -100.0f;

            float& value = smoothed[(size_t) i];

            if (! primed)
                value = target;
            else
                value += (target - value) * (target > value ? 0.55f : 0.16f);
        }

        primed = true;
    }

    repaint();
}

void SpectrumDisplay::paint (juce::Graphics& g)
{
    auto area = getLocalBounds().toFloat();

    g.setColour (panel);
    g.fillRoundedRectangle (area, 10.0f);
    g.setColour (stroke.withAlpha (0.85f));
    g.drawRoundedRectangle (area.reduced (0.5f), 10.0f, 1.0f);

    auto inner = area.reduced (16.0f, 14.0f);
    auto plot  = inner.withTrimmedBottom (16.0f);

    // grid
    const float freqs[] = { 50.0f, 100.0f, 200.0f, 500.0f, 1000.0f, 2000.0f, 5000.0f, 10000.0f };
    g.setFont (font (8.5f));

    for (float f : freqs)
    {
        const float x = xForFreq (f, plot);
        g.setColour (stroke.withAlpha (0.45f));
        g.drawVerticalLine ((int) x, plot.getY(), plot.getBottom());

        const juce::String label = f >= 1000.0f ? juce::String (f / 1000.0f, 0) + "k"
                                                : juce::String ((int) f);
        g.setColour (muted.withAlpha (0.75f));
        g.drawText (label, juce::Rectangle<float> (x - 18.0f, plot.getBottom() + 2.0f, 36.0f, 14.0f),
                    juce::Justification::centred, false);
    }

    for (int db = -12; db >= -72; db -= 12)
    {
        const float y = yForDb ((float) db, plot);
        g.setColour (stroke.withAlpha (0.35f));
        g.drawHorizontalLine ((int) y, plot.getX(), plot.getRight());
        g.setColour (muted.withAlpha (0.6f));
        g.drawText (juce::String (db), juce::Rectangle<float> (plot.getX(), y - 7.0f, 26.0f, 14.0f),
                    juce::Justification::centredLeft, false);
    }

    // spectrum
    juce::Path curve;
    bool started = false;

    const int steps = juce::jmax (2, (int) plot.getWidth());

    for (int i = 0; i <= steps; ++i)
    {
        const float t = (float) i / (float) steps;
        const float freq = 20.0f * std::pow (1000.0f, t);
        const float bin = freq * (float) defb::Engine::kFftSize
                            / juce::jmax (8000.0f, processor.getEngine().getDisplaySampleRate());
        const int index = juce::jlimit (0, (int) smoothed.size() - 1, (int) std::lround (bin));

        const float x = plot.getX() + t * plot.getWidth();
        const float y = yForDb (smoothed[(size_t) index], plot);

        if (! started)
        {
            curve.startNewSubPath (x, y);
            started = true;
        }
        else
        {
            curve.lineTo (x, y);
        }
    }

    if (started)
    {
        auto filled = curve;
        filled.lineTo (plot.getRight(), plot.getBottom());
        filled.lineTo (plot.getX(), plot.getBottom());
        filled.closeSubPath();

        g.setGradientFill (juce::ColourGradient (accent.withAlpha (0.30f), plot.getX(), plot.getY(),
                                                 accent.withAlpha (0.015f), plot.getX(), plot.getBottom(),
                                                 false));
        g.fillPath (filled);

        g.setColour (accent.withAlpha (0.85f));
        g.strokePath (curve, juce::PathStrokeType (1.3f));
    }

    // active notches
    defb::NotchInfo info[defb::Engine::kMaxNotches] {};
    const int numNotches = processor.getEngine().getNotchInfo (info, defb::Engine::kMaxNotches);

    g.setFont (font (9.0f, true));

    for (int i = 0; i < numNotches; ++i)
    {
        const float x = xForFreq (info[i].freq, plot);

        if (x < plot.getX() - 4.0f || x > plot.getRight() + 4.0f)
            continue;

        auto band = juce::Rectangle<float> (x - 9.0f, plot.getY(), 18.0f, plot.getHeight());
        g.setColour (warn.withAlpha (0.10f));
        g.fillRoundedRectangle (band, 3.0f);

        g.setColour (warn.withAlpha (0.9f));
        g.drawVerticalLine ((int) x, plot.getY(), plot.getBottom());

        const juce::String label = info[i].freq >= 1000.0f
                                     ? juce::String (info[i].freq / 1000.0f, 2) + "k"
                                     : juce::String ((int) info[i].freq);

        g.drawText (label, juce::Rectangle<float> (x - 26.0f, plot.getY() + 4.0f, 52.0f, 12.0f),
                    juce::Justification::centred, false);
    }

    // state caption
    const int mode = (int) std::lround (processor.getApvts().getRawParameterValue ("mode")->load());

    if (mode == 0)
    {
        g.setFont (font (11.0f, true));
        g.setColour (muted.withAlpha (0.65f));
        g.drawText ("BYPASSED", plot, juce::Justification::centred, false);
    }
    else if (numNotches == 0)
    {
        g.setFont (font (10.0f, true));
        g.setColour (muted.withAlpha (0.55f));
        g.drawText ("LISTENING", plot, juce::Justification::centred, false);
    }
}

//==============================================================================
DeFeedbackAudioProcessorEditor::DeFeedbackAudioProcessorEditor (DeFeedbackAudioProcessor& p)
    : AudioProcessorEditor (&p), processor (p), spectrum (p)
{
    setLookAndFeel (&lookAndFeel);

    addKnob ("sensitivity", "SENSITIVITY", [] (double v) { return juce::String (v, 1); });
    addKnob ("maxNotches",  "NOTCHES",     [] (double v) { return juce::String ((int) std::lround (v)); });
    addKnob ("depth",       "DEPTH",       [] (double v) { return juce::String ((int) std::lround (v)); });
    addKnob ("width",       "WIDTH",       [] (double v) { return juce::String ((int) std::lround (v)); });
    addKnob ("attack",      "ATTACK",      [] (double v) { return v < 999.0 ? juce::String ((int) std::lround (v))
                                                                           : juce::String (v / 1000.0, 1) + "s"; });
    addKnob ("release",     "RELEASE",     [] (double v) { return v < 999.0 ? juce::String ((int) std::lround (v))
                                                                           : juce::String (v / 1000.0, 1) + "s"; });
    addKnob ("lowFreq",     "LOW",         [] (double v) { return juce::String ((int) std::lround (v)); });
    addKnob ("highFreq",    "HIGH",        [] (double v) { return v >= 1000.0 ? juce::String (v / 1000.0, 1) + "k"
                                                                             : juce::String ((int) std::lround (v)); });

    addAndMakeVisible (spectrum);

    const juce::StringArray names { "OFF", "HUNT", "DYNAMIC" };

    for (int i = 0; i < 3; ++i)
    {
        modeButtons[i].setButtonText (names[i]);
        modeButtons[i].setClickingTogglesState (false);
        modeButtons[i].onClick = [this, i]
        {
            auto* param = processor.getApvts().getParameter ("mode");
            param->setValueNotifyingHost (param->convertTo0to1 ((float) i));
            updateModeButtons();
            spectrum.repaint();
        };
        addAndMakeVisible (modeButtons[i]);
    }

    guardButton.setClickingTogglesState (true);
    addAndMakeVisible (guardButton);
    guardAttachment = std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment> (
        processor.getApvts(), "musicGuard", guardButton);

    clearButton.onClick = [this] { processor.clearNotches(); spectrum.repaint(); };
    addAndMakeVisible (clearButton);

    statusLabel.setFont (font (10.0f));
    statusLabel.setColour (juce::Label::textColourId, muted);
    statusLabel.setJustificationType (juce::Justification::centredLeft);
    addAndMakeVisible (statusLabel);

    setResizable (true, true);
    setResizeLimits (672, 464, 1680, 1160);
    if (auto* constrainer = getConstrainer())
        constrainer->setFixedAspectRatio (840.0 / 580.0);

    setSize (840, 580);
    startTimerHz (30);
}

DeFeedbackAudioProcessorEditor::~DeFeedbackAudioProcessorEditor()
{
    stopTimer();
    setLookAndFeel (nullptr);
}

void DeFeedbackAudioProcessorEditor::addKnob (const juce::String& paramID, const juce::String& name,
                                              std::function<juce::String (double)> format)
{
    auto knob = std::make_unique<Knob>();

    knob->slider.setSliderStyle (juce::Slider::RotaryHorizontalVerticalDrag);
    knob->slider.setTextBoxStyle (juce::Slider::NoTextBox, false, 0, 0);
    knob->slider.setRotaryParameters (juce::MathConstants<float>::pi * 1.2f,
                                      juce::MathConstants<float>::pi * 2.8f, true);
    knob->slider.textFromValueFunction = std::move (format);

    knob->label.setText (name, juce::dontSendNotification);
    knob->label.setFont (font (9.0f, true));
    knob->label.setJustificationType (juce::Justification::centred);
    knob->label.setColour (juce::Label::textColourId, muted);

    addAndMakeVisible (knob->slider);
    addAndMakeVisible (knob->label);

    knob->attachment = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment> (
        processor.getApvts(), paramID, knob->slider);

    knobs.push_back (std::move (knob));
}

void DeFeedbackAudioProcessorEditor::paint (juce::Graphics& g)
{
    const float w = (float) getWidth();
    const float h = (float) getHeight();
    const float u = w / 840.0f;

    g.setGradientFill (juce::ColourGradient (bg.brighter (0.035f), 0.0f, 0.0f,
                                             bg.darker (0.25f), 0.0f, h, false));
    g.fillAll();

    // header
    g.setFont (font (23.0f * u, true));
    g.setColour (text);
    g.drawText ("DE", juce::Rectangle<float> (24.0f * u, 16.0f * u, 60.0f * u, 30.0f * u),
                juce::Justification::centredLeft, false);

    g.setColour (accent);
    g.drawText ("FEEDBACK",
                juce::Rectangle<float> (80.0f * u, 16.0f * u, 220.0f * u, 30.0f * u),
                juce::Justification::centredLeft, false);

    g.setFont (font (8.5f * u, true));
    g.setColour (muted);
    g.drawText ("ADAPTIVE FEEDBACK SUPPRESSOR",
                juce::Rectangle<float> (24.0f * u, 46.0f * u, 300.0f * u, 14.0f * u),
                juce::Justification::centredLeft, false);

    g.setColour (accent.withAlpha (0.85f));
    g.fillRect (juce::Rectangle<float> (24.0f * u, 66.0f * u, 46.0f * u, 2.0f * u));

    g.setColour (stroke.withAlpha (0.65f));
    g.drawHorizontalLine ((int) (74.0f * u), 24.0f * u, w - 24.0f * u);

    // footer rule
    g.setColour (stroke.withAlpha (0.5f));
    g.drawHorizontalLine ((int) (h - 104.0f * u), 24.0f * u, w - 24.0f * u);
}

void DeFeedbackAudioProcessorEditor::resized()
{
    const float w = (float) getWidth();
    const float h = (float) getHeight();
    const float u = w / 840.0f;

    spectrum.setBounds (juce::Rectangle<float> (24.0f * u, 84.0f * u,
                                                w - 48.0f * u, 264.0f * u).toNearestInt());

    // main knob row
    const float top   = 362.0f * u;
    const float size  = 76.0f * u;
    const float cell  = (w - 48.0f * u) / 6.0f;

    for (int i = 0; i < 6; ++i)
    {
        const float cx = 24.0f * u + cell * ((float) i + 0.5f);
        knobs[(size_t) i]->slider.setBounds (
            juce::Rectangle<float> (cx - size * 0.5f, top, size, size).toNearestInt());
        knobs[(size_t) i]->label.setBounds (
            juce::Rectangle<float> (cx - cell * 0.5f, top + size + 2.0f * u, cell, 14.0f * u).toNearestInt());
    }

    // small range knobs
    const float bottomTop = h - 88.0f * u;
    const float small = 50.0f * u;

    for (int i = 6; i < 8; ++i)
    {
        const float cx = 24.0f * u + small * 0.5f + (float) (i - 6) * (small + 26.0f * u);
        knobs[(size_t) i]->slider.setBounds (
            juce::Rectangle<float> (cx - small * 0.5f, bottomTop, small, small).toNearestInt());
        knobs[(size_t) i]->label.setBounds (
            juce::Rectangle<float> (cx - 26.0f * u, bottomTop + small + 2.0f * u, 52.0f * u, 12.0f * u).toNearestInt());
    }

    guardButton.setBounds (juce::Rectangle<float> (168.0f * u, bottomTop + 12.0f * u,
                                                   132.0f * u, 26.0f * u).toNearestInt());

    const float clearW = 64.0f * u;
    clearButton.setBounds (juce::Rectangle<float> (w - 24.0f * u - clearW, bottomTop + 12.0f * u,
                                                   clearW, 26.0f * u).toNearestInt());

    statusLabel.setBounds (juce::Rectangle<float> (318.0f * u, bottomTop + 12.0f * u,
                                                   w - 318.0f * u - 24.0f * u - clearW - 12.0f * u,
                                                   26.0f * u).toNearestInt());

    // mode buttons (header, right)
    const float bw = 76.0f * u;
    const float bh = 27.0f * u;
    const float gap = 6.0f * u;
    const float startX = w - 24.0f * u - (bw * 3.0f + gap * 2.0f);

    for (int i = 0; i < 3; ++i)
        modeButtons[i].setBounds (juce::Rectangle<float> (startX + (float) i * (bw + gap),
                                                          26.0f * u, bw, bh).toNearestInt());
}

void DeFeedbackAudioProcessorEditor::timerCallback()
{
    updateModeButtons();
    updateStatus();
    spectrum.refresh();
}

void DeFeedbackAudioProcessorEditor::updateModeButtons()
{
    const int mode = (int) std::lround (processor.getApvts().getRawParameterValue ("mode")->load());

    for (int i = 0; i < 3; ++i)
        modeButtons[i].setToggleState (i == mode, juce::dontSendNotification);
}

void DeFeedbackAudioProcessorEditor::updateStatus()
{
    const int mode = (int) std::lround (processor.getApvts().getRawParameterValue ("mode")->load());

    if (mode == 0)
    {
        statusLabel.setText ("BYPASSED  ·  NO PROCESSING", juce::dontSendNotification);
        return;
    }

    defb::NotchInfo info[defb::Engine::kMaxNotches] {};
    const int count = processor.getEngine().getNotchInfo (info, defb::Engine::kMaxNotches);

    if (count == 0)
    {
        statusLabel.setText (mode == 2 ? "LISTENING  ·  DYNAMIC" : "LISTENING  ·  NO FEEDBACK",
                             juce::dontSendNotification);
        return;
    }

    juce::String text;
    text << count << (count == 1 ? " NOTCH  ·  " : " NOTCHES  ·  ");

    for (int i = 0; i < count && i < 5; ++i)
    {
        if (i > 0)
            text << "  ";

        text << (info[i].freq >= 1000.0 ? juce::String (info[i].freq / 1000.0f, 2) + "k"
                                        : juce::String ((int) std::lround (info[i].freq)));
    }

    text << " Hz";
    statusLabel.setText (text, juce::dontSendNotification);
}
