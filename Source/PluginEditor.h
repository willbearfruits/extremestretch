#pragma once

#include "PluginProcessor.h"
#include "Ui.h"

namespace es
{

class ExtremeStretchEditor final : public juce::AudioProcessorEditor,
                                   public juce::FileDragAndDropTarget,
                                   private juce::Timer
{
public:
    explicit ExtremeStretchEditor (ExtremeStretchProcessor&);
    ~ExtremeStretchEditor() override;

    void paint (juce::Graphics&) override;
    void resized() override;

    bool isInterestedInFileDrag (const juce::StringArray& files) override;
    void filesDropped (const juce::StringArray& files, int x, int y) override;

private:
    void timerCallback() override;
    void chooseFile();
    void startRender();
    void applyTargetLength();
    void algorithmChanged (Algorithm);
    void attach (ui::Knob& knob, const char* parameterID);
    void attach (juce::Button& button, const char* parameterID);
    void say (const juce::String& message, bool isError);

    using SliderAttachment   = juce::AudioProcessorValueTreeState::SliderAttachment;
    using ButtonAttachment   = juce::AudioProcessorValueTreeState::ButtonAttachment;
    using ComboBoxAttachment = juce::AudioProcessorValueTreeState::ComboBoxAttachment;

    ExtremeStretchProcessor& processor;
    ui::LookAndFeel          lnf;

    ui::WaveformView waveform;
    ui::SpectrumView spectrum;

    ui::Knob stretchKnob  { "STRETCH",  ui::colours::cold };
    ui::Knob windowKnob   { "WINDOW",   ui::colours::cold };
    ui::Knob shapeKnob    { "SHAPE",    ui::colours::cold };
    ui::Knob pitchKnob    { "PITCH",    ui::colours::cold };
    ui::Knob widthKnob    { "WIDTH",    ui::colours::cold };
    ui::Knob blurKnob     { "BLUR",     ui::colours::hot };
    ui::Knob rotKnob      { "ROT",      ui::colours::hot };
    ui::Knob scrambleKnob { "SCRAMBLE", ui::colours::hot };
    ui::Knob lowCutKnob   { "LOW CUT",  ui::colours::dim };
    ui::Knob highCutKnob  { "HIGH CUT", ui::colours::dim };
    ui::Knob gainKnob     { "GAIN",     ui::colours::dim };

    juce::TextButton loadButton   { "LOAD" };
    juce::TextButton renderButton { "RENDER" };
    juce::TextButton playButton   { "PLAY" };
    juce::TextButton freezeButton { "FREEZE" };
    juce::TextButton loopButton   { "LOOP" };
    juce::TextButton liveButton   { "LIVE IN" };

    juce::ComboBox   algorithmBox;
    juce::TextEditor lengthEditor;

    int paulstretchCaptionY = 0;
    int destructionCaptionY = 0;
    juce::Rectangle<int> lengthRow;

    Algorithm    currentAlgorithm = Algorithm::smooth;
    juce::String sourceText { "no source" };
    juce::String statusText;
    juce::String messageText;
    bool         messageIsError = false;
    int          messageTicks   = 0;

    std::vector<std::unique_ptr<SliderAttachment>>   sliderAttachments;
    std::vector<std::unique_ptr<ButtonAttachment>>   buttonAttachments;
    std::unique_ptr<ComboBoxAttachment>              algorithmAttachment;

    std::unique_ptr<juce::FileChooser>              chooser;
    std::unique_ptr<juce::ThreadWithProgressWindow> renderJob;

    bool wasLive = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ExtremeStretchEditor)
};

} // namespace es
