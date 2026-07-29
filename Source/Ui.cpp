#include "Ui.h"

#include <cmath>

namespace es::ui
{

//==============================================================================
LookAndFeel::LookAndFeel()
{
    setColour (juce::ResizableWindow::backgroundColourId, colours::bg);
    setColour (juce::Slider::textBoxTextColourId,         colours::text);
    setColour (juce::Slider::textBoxOutlineColourId,      juce::Colours::transparentBlack);
    setColour (juce::Slider::textBoxBackgroundColourId,   juce::Colours::transparentBlack);
    setColour (juce::Label::textColourId,                 colours::text);
    setColour (juce::TextButton::buttonColourId,          colours::panel);
    setColour (juce::TextButton::textColourOffId,         colours::text);
    setColour (juce::TextButton::textColourOnId,          colours::bg);
    setColour (juce::ComboBox::backgroundColourId,        colours::panel);
    setColour (juce::ComboBox::textColourId,              colours::text);
    setColour (juce::ComboBox::outlineColourId,           colours::edge);
    setColour (juce::PopupMenu::backgroundColourId,       colours::panel);
    setColour (juce::PopupMenu::textColourId,             colours::text);
    setColour (juce::AlertWindow::backgroundColourId,     colours::panel);
    setColour (juce::AlertWindow::textColourId,           colours::text);
    setColour (juce::AlertWindow::outlineColourId,        colours::edge);
}

juce::Font LookAndFeel::getLabelFont (juce::Label& label)
{
    return { juce::FontOptions ((float) juce::jmin (14, label.getHeight() - 2)) };
}

juce::Font LookAndFeel::getTextButtonFont (juce::TextButton&, int buttonHeight)
{
    return { juce::FontOptions ((float) juce::jmin (14, buttonHeight - 8)).withStyle ("Bold") };
}

void LookAndFeel::drawRotarySlider (juce::Graphics& g, int x, int y, int width, int height,
                                    float sliderPos, float startAngle, float endAngle,
                                    juce::Slider& slider)
{
    const auto bounds = juce::Rectangle<int> (x, y, width, height).toFloat().reduced (3.0f);
    const auto radius = juce::jmin (bounds.getWidth(), bounds.getHeight()) * 0.5f;
    const auto centre = bounds.getCentre();
    const auto thick  = juce::jmax (2.5f, radius * 0.16f);
    const auto angle  = startAngle + sliderPos * (endAngle - startAngle);

    const auto accent = slider.findColour (juce::Slider::rotarySliderFillColourId);

    juce::Path track;
    track.addCentredArc (centre.x, centre.y, radius - thick, radius - thick,
                         0.0f, startAngle, endAngle, true);
    g.setColour (colours::edge);
    g.strokePath (track, juce::PathStrokeType (thick, juce::PathStrokeType::curved,
                                               juce::PathStrokeType::rounded));

    if (sliderPos > 0.001f)
    {
        juce::Path value;
        value.addCentredArc (centre.x, centre.y, radius - thick, radius - thick,
                             0.0f, startAngle, angle, true);
        g.setColour (accent);
        g.strokePath (value, juce::PathStrokeType (thick, juce::PathStrokeType::curved,
                                                   juce::PathStrokeType::rounded));
    }

    juce::Path pointer;
    const auto inner = radius - thick * 2.4f;
    pointer.startNewSubPath (centre.x + inner * 0.35f * std::sin (angle),
                             centre.y - inner * 0.35f * std::cos (angle));
    pointer.lineTo (centre.x + inner * std::sin (angle),
                    centre.y - inner * std::cos (angle));

    g.setColour (colours::text);
    g.strokePath (pointer, juce::PathStrokeType (juce::jmax (1.5f, thick * 0.5f),
                                                 juce::PathStrokeType::curved,
                                                 juce::PathStrokeType::rounded));
}

void LookAndFeel::drawButtonBackground (juce::Graphics& g, juce::Button& button,
                                        const juce::Colour& backgroundColour,
                                        bool highlighted, bool down)
{
    const auto r = button.getLocalBounds().toFloat().reduced (0.5f);
    const bool on = button.getToggleState();

    auto fill = on ? button.findColour (juce::TextButton::buttonOnColourId)
                   : backgroundColour;

    if (down)             fill = fill.brighter (0.25f);
    else if (highlighted) fill = fill.brighter (0.12f);

    g.setColour (fill);
    g.fillRoundedRectangle (r, 3.0f);

    g.setColour (on ? fill.brighter (0.4f) : colours::edge);
    g.drawRoundedRectangle (r, 3.0f, 1.0f);
}

void LookAndFeel::drawToggleButton (juce::Graphics& g, juce::ToggleButton& button,
                                    bool highlighted, bool down)
{
    drawButtonBackground (g, button, button.findColour (juce::TextButton::buttonColourId),
                          highlighted, down);

    g.setColour (button.getToggleState() ? colours::bg : colours::text);
    g.setFont (juce::FontOptions (12.0f).withStyle ("Bold"));
    g.drawText (button.getButtonText(), button.getLocalBounds(), juce::Justification::centred);
}

//==============================================================================
Knob::Knob (juce::String caption, juce::Colour accent)
    : name (std::move (caption)), accentColour (accent)
{
    slider.setSliderStyle (juce::Slider::RotaryHorizontalVerticalDrag);
    slider.setTextBoxStyle (juce::Slider::TextBoxBelow, false, 74, 16);
    slider.setColour (juce::Slider::rotarySliderFillColourId, accent);
    slider.setColour (juce::Slider::textBoxTextColourId, colours::text);
    addAndMakeVisible (slider);
}

void Knob::setCaption (juce::String newCaption)
{
    if (name == newCaption)
        return;

    name = std::move (newCaption);
    repaint();
}

void Knob::setActive (bool shouldBeActive)
{
    if (active == shouldBeActive)
        return;

    active = shouldBeActive;

    slider.setColour (juce::Slider::rotarySliderFillColourId,
                      active ? accentColour : colours::edge);
    slider.setColour (juce::Slider::textBoxTextColourId,
                      active ? colours::text : colours::dim.withAlpha (0.5f));
    slider.setEnabled (active);
    repaint();
}

void Knob::paint (juce::Graphics& g)
{
    g.setColour (active ? colours::dim : colours::dim.withAlpha (0.4f));
    g.setFont (juce::FontOptions (10.5f).withStyle ("Bold"));
    g.drawText (name, getLocalBounds().removeFromTop (13), juce::Justification::centred);
}

void Knob::resized()
{
    slider.setBounds (getLocalBounds().withTrimmedTop (13));
}

//==============================================================================
WaveformView::WaveformView (StretchEngine& e) : engine (e)
{
    setMouseCursor (juce::MouseCursor::PointingHandCursor);
    startTimerHz (24);
}

void WaveformView::mouseDown (const juce::MouseEvent& e) { seekFrom (e); }
void WaveformView::mouseDrag (const juce::MouseEvent& e) { seekFrom (e); }

void WaveformView::seekFrom (const juce::MouseEvent& e)
{
    if (onSeek == nullptr || getWidth() <= 0 || ! engine.hasSource())
        return;

    onSeek (juce::jlimit (0.0, 1.0, (double) e.position.x / (double) getWidth()));
}

void WaveformView::sourceChanged()
{
    summarisedFor = -1;
    rebuildPeaks();
    repaint();
}

void WaveformView::resized()
{
    rebuildPeaks();
}

void WaveformView::timerCallback()
{
    const auto p = (float) engine.getPlaybackProgress();

    if (std::abs (p - lastProgress) > 0.0005f)
    {
        lastProgress = p;
        repaint();
    }
}

void WaveformView::rebuildPeaks()
{
    const auto& buffer = engine.getSourceBufferForDisplay();
    const int   width  = juce::jmax (1, getWidth());
    const int   len    = buffer.getNumSamples();

    if (len == summarisedFor && (int) peaks.size() == width)
        return;

    summarisedFor = len;
    peaks.assign ((size_t) width, juce::Range<float> (0.0f, 0.0f));

    if (len <= 0 || buffer.getNumChannels() <= 0)
        return;

    for (int x = 0; x < width; ++x)
    {
        const auto start = (int) ((juce::int64) x       * len / width);
        const auto end   = (int) ((juce::int64) (x + 1) * len / width);

        auto range = buffer.findMinMax (0, start, juce::jmax (1, end - start));

        for (int ch = 1; ch < buffer.getNumChannels(); ++ch)
            range = range.getUnionWith (buffer.findMinMax (ch, start, juce::jmax (1, end - start)));

        peaks[(size_t) x] = range;
    }
}

void WaveformView::paint (juce::Graphics& g)
{
    const auto r = getLocalBounds().toFloat();

    g.setColour (colours::panel);
    g.fillRoundedRectangle (r, 3.0f);

    rebuildPeaks();

    const auto mid = r.getCentreY();
    const auto half = r.getHeight() * 0.46f;

    if (engine.hasSource() && ! peaks.empty())
    {
        g.setColour (colours::dim.withAlpha (0.85f));

        for (int x = 0; x < (int) peaks.size(); ++x)
        {
            const auto& p = peaks[(size_t) x];
            const auto top = mid - juce::jlimit (-1.0f, 1.0f, p.getEnd())   * half;
            const auto bot = mid - juce::jlimit (-1.0f, 1.0f, p.getStart()) * half;
            g.drawVerticalLine (x, juce::jmin (top, bot), juce::jmax (bot, top) + 0.8f);
        }

        const auto progress = (float) engine.getPlaybackProgress();

        if (progress > 0.0f)
        {
            g.setColour (colours::cold.withAlpha (0.16f));
            g.fillRect (r.withWidth (r.getWidth() * progress));
            g.setColour (colours::cold);
            g.fillRect (juce::Rectangle<float> (r.getWidth() * progress - 0.75f, r.getY(), 1.5f, r.getHeight()));
        }
    }
    else
    {
        g.setColour (colours::dim);
        g.setFont (juce::FontOptions (13.0f));
        g.drawText (engine.getSourceMode() == StretchEngine::SourceMode::live
                        ? "LIVE INPUT"
                        : "drop an audio file here",
                    getLocalBounds(), juce::Justification::centred);
    }

    g.setColour (colours::edge);
    g.drawRoundedRectangle (r.reduced (0.5f), 3.0f, 1.0f);
}

//==============================================================================
SpectrumView::SpectrumView (StretchEngine& e) : engine (e)
{
    values  .assign ((size_t) PaulStretch::displayBins, 0.0f);
    smoothed.assign ((size_t) PaulStretch::displayBins, 0.0f);
    startTimerHz (30);
}

void SpectrumView::timerCallback()
{
    engine.copyDisplaySpectrum (values.data(), (int) values.size());

    for (size_t i = 0; i < smoothed.size(); ++i)
    {
        const auto target = values[i];
        auto& s = smoothed[i];
        s = target > s ? target : s + (target - s) * 0.25f;   // fast attack, slow release
    }

    repaint();
}

void SpectrumView::paint (juce::Graphics& g)
{
    const auto r = getLocalBounds().toFloat();

    g.setColour (colours::panel);
    g.fillRoundedRectangle (r, 3.0f);

    const auto n = (int) smoothed.size();
    const auto w = r.getWidth() / (float) n;

    for (int i = 0; i < n; ++i)
    {
        const auto db     = juce::Decibels::gainToDecibels (smoothed[(size_t) i], -80.0f);
        const auto height = juce::jlimit (0.0f, 1.0f, (db + 80.0f) / 80.0f) * r.getHeight();

        if (height < 0.5f)
            continue;

        const auto hue = juce::jmap ((float) i / (float) n, 0.5f, 0.98f);

        g.setColour (colours::cold.interpolatedWith (colours::hot, hue).withAlpha (0.9f));
        g.fillRect (juce::Rectangle<float> (r.getX() + (float) i * w, r.getBottom() - height,
                                            juce::jmax (1.0f, w - 0.6f), height));
    }

    g.setColour (colours::edge);
    g.drawRoundedRectangle (r.reduced (0.5f), 3.0f, 1.0f);
}

} // namespace es::ui
