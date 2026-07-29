#pragma once

#include "StretchEngine.h"

#include <juce_gui_basics/juce_gui_basics.h>

namespace es::ui
{

namespace colours
{
    const juce::Colour bg      { 0xff0a0b0d };
    const juce::Colour panel   { 0xff131518 };
    const juce::Colour edge    { 0xff262b32 };
    const juce::Colour text    { 0xffd8dce2 };
    const juce::Colour dim     { 0xff69707a };
    const juce::Colour hot     { 0xffff4433 };  // the destructive controls
    const juce::Colour cold    { 0xff31e0c0 };  // the paulstretch controls
}

//==============================================================================
class LookAndFeel final : public juce::LookAndFeel_V4
{
public:
    LookAndFeel();

    void drawRotarySlider (juce::Graphics&, int x, int y, int width, int height,
                           float sliderPos, float rotaryStartAngle, float rotaryEndAngle,
                           juce::Slider&) override;

    void drawButtonBackground (juce::Graphics&, juce::Button&, const juce::Colour&,
                               bool shouldDrawAsHighlighted, bool shouldDrawAsDown) override;

    void drawToggleButton (juce::Graphics&, juce::ToggleButton&,
                           bool shouldDrawAsHighlighted, bool shouldDrawAsDown) override;

    juce::Font getLabelFont (juce::Label&) override;
    juce::Font getTextButtonFont (juce::TextButton&, int buttonHeight) override;
};

//==============================================================================
/** A rotary with its caption drawn above and its value below. */
class Knob final : public juce::Component
{
public:
    Knob (juce::String caption, juce::Colour accent);

    void paint (juce::Graphics&) override;
    void resized() override;

    /** SHAPE means something different per algorithm, so its caption moves. */
    void setCaption (juce::String newCaption);

    /** Greys out controls the current algorithm does not use. */
    void setActive (bool shouldBeActive);

    juce::Slider slider;

private:
    juce::String name;
    juce::Colour accentColour;
    bool         active = true;
};

//==============================================================================
/** Source waveform with a playhead. */
class WaveformView final : public juce::Component,
                           private juce::Timer
{
public:
    explicit WaveformView (StretchEngine&);

    void paint (juce::Graphics&) override;
    void resized() override;

    /** Recomputes the min/max summary. Call after loading. */
    void sourceChanged();

    /** Click or drag anywhere on the waveform to jump there. */
    std::function<void (double)> onSeek;

    void mouseDown (const juce::MouseEvent&) override;
    void mouseDrag (const juce::MouseEvent&) override;

private:
    void timerCallback() override;
    void rebuildPeaks();
    void seekFrom (const juce::MouseEvent&);

    StretchEngine& engine;
    std::vector<juce::Range<float>> peaks;
    int   summarisedFor = -1;
    float lastProgress  = -1.0f;
};

//==============================================================================
/** Log-frequency view of the frame the engine just emitted. */
class SpectrumView final : public juce::Component,
                           private juce::Timer
{
public:
    explicit SpectrumView (StretchEngine&);

    void paint (juce::Graphics&) override;

private:
    void timerCallback() override;

    StretchEngine&     engine;
    std::vector<float> values, smoothed;
};

} // namespace es::ui
