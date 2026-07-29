#include "PluginEditor.h"
#include "Duration.h"

namespace es
{

namespace
{
    constexpr double maxRenderSeconds = 30.0 * 60.0;

    /** Runs StretchEngine::renderToFile on its own thread behind a progress
        window. launchThread() is used rather than runThread() because a plugin
        editor must never spin a modal loop. */
    class RenderJob final : public juce::ThreadWithProgressWindow
    {
    public:
        RenderJob (StretchEngine& e, juce::File dest, StretchParams p)
            : juce::ThreadWithProgressWindow ("Rendering the stretch", true, true),
              engine (e), destination (std::move (dest)), params (p)
        {
            setStatusMessage ("Writing " + destination.getFileName() + "...");
        }

        void run() override
        {
            result = engine.renderToFile (destination, maxRenderSeconds, params,
                                          [this] (double progress)
                                          {
                                              setProgress (progress);
                                              return ! threadShouldExit();
                                          });
        }

        void threadComplete (bool userPressedCancel) override
        {
            if (onFinished != nullptr)
                onFinished (userPressedCancel ? juce::String ("Render cancelled.") : result,
                            destination);
        }

        std::function<void (juce::String, juce::File)> onFinished;

    private:
        StretchEngine& engine;
        juce::File     destination;
        StretchParams  params;
        juce::String   result;
    };

}

//==============================================================================

ExtremeStretchEditor::ExtremeStretchEditor (ExtremeStretchProcessor& p)
    : juce::AudioProcessorEditor (&p),
      processor (p),
      waveform (p.getEngine()),
      spectrum (p.getEngine())
{
    setLookAndFeel (&lnf);

    addAndMakeVisible (waveform);
    addAndMakeVisible (spectrum);

    for (auto* knob : { &stretchKnob, &windowKnob, &shapeKnob, &pitchKnob, &widthKnob,
                        &blurKnob, &rotKnob, &scrambleKnob,
                        &lowCutKnob, &highCutKnob, &gainKnob })
        addAndMakeVisible (*knob);

    attach (stretchKnob,  pid::stretch);
    attach (windowKnob,   pid::window);
    attach (shapeKnob,    pid::shape);
    attach (pitchKnob,    pid::pitch);
    attach (widthKnob,    pid::width);
    attach (blurKnob,     pid::blur);
    attach (rotKnob,      pid::rot);
    attach (scrambleKnob, pid::scramble);
    attach (lowCutKnob,   pid::lowCut);
    attach (highCutKnob,  pid::highCut);
    attach (gainKnob,     pid::gain);

    for (auto* button : { &loadButton, &renderButton, &playButton,
                          &freezeButton, &loopButton, &liveButton })
        addAndMakeVisible (*button);

    for (auto* toggle : { &freezeButton, &loopButton, &liveButton })
    {
        toggle->setClickingTogglesState (true);
        toggle->setColour (juce::TextButton::buttonOnColourId, ui::colours::hot);
    }

    freezeButton.setColour (juce::TextButton::buttonOnColourId, ui::colours::cold);
    playButton.setColour (juce::TextButton::buttonOnColourId, ui::colours::cold);

    attach (freezeButton, pid::freeze);
    attach (loopButton,   pid::loop);
    attach (liveButton,   pid::live);

    addAndMakeVisible (algorithmBox);

    for (int i = 0; i < numAlgorithms; ++i)
        algorithmBox.addItem (toString ((Algorithm) i), i + 1);

    algorithmAttachment = std::make_unique<ComboBoxAttachment> (processor.apvts, pid::algorithm,
                                                               algorithmBox);

    addAndMakeVisible (lengthEditor);
    lengthEditor.setJustification (juce::Justification::centredLeft);
    lengthEditor.setColour (juce::TextEditor::backgroundColourId, ui::colours::panel);
    lengthEditor.setColour (juce::TextEditor::outlineColourId,    ui::colours::edge);
    lengthEditor.setColour (juce::TextEditor::focusedOutlineColourId, ui::colours::cold);
    lengthEditor.setColour (juce::TextEditor::textColourId,       ui::colours::text);
    lengthEditor.setColour (juce::TextEditor::highlightColourId,  ui::colours::cold.withAlpha (0.3f));
    lengthEditor.setFont (juce::FontOptions (13.0f));
    lengthEditor.setTooltip ("Type how long the whole thing should last, e.g. 3h, 45 min, 2.5 billion years");
    lengthEditor.onReturnKey = [this] { applyTargetLength(); };

    // Clicking or dragging the waveform jumps there.
    waveform.onSeek = [this] (double position)
    {
        processor.getEngine().seekToSourceNormalised (position);
    };

    loadButton.onClick   = [this] { chooseFile(); };
    renderButton.onClick = [this] { startRender(); };

    playButton.onClick = [this]
    {
        auto& engine = processor.getEngine();

        if (engine.isPlaying())
            engine.stop();
        else
            engine.start();
    };

    setResizable (true, true);
    setResizeLimits (900, 620, 1800, 1200);
    setSize (1000, 680);

    algorithmChanged (Algorithm::smooth);
    startTimerHz (12);
}

ExtremeStretchEditor::~ExtremeStretchEditor()
{
    setLookAndFeel (nullptr);
}

void ExtremeStretchEditor::attach (ui::Knob& knob, const char* parameterID)
{
    sliderAttachments.push_back (
        std::make_unique<SliderAttachment> (processor.apvts, parameterID, knob.slider));
}

void ExtremeStretchEditor::attach (juce::Button& button, const char* parameterID)
{
    buttonAttachments.push_back (
        std::make_unique<ButtonAttachment> (processor.apvts, parameterID, button));
}

void ExtremeStretchEditor::algorithmChanged (Algorithm algorithm)
{
    currentAlgorithm = algorithm;

    // SHAPE is one knob wearing five hats.
    shapeKnob.setCaption (shapeCaptionFor (algorithm));

    // Grain works in the time domain, so nothing spectral applies to it.
    const bool spectral = isSpectral (algorithm);

    for (auto* knob : { &blurKnob, &rotKnob, &scrambleKnob, &lowCutKnob, &highCutKnob })
        knob->setActive (spectral);

    repaint();
}

void ExtremeStretchEditor::applyTargetLength()
{
    auto& engine = processor.getEngine();

    if (! engine.hasSource())
    {
        say ("Load a file before setting a length.", true);
        return;
    }

    const double wanted = duration::parse (lengthEditor.getText());

    if (wanted <= 0.0)
    {
        say (R"(Didn't understand that - try "3h", "45 min", "2:30", or "2.5 billion years".)", true);
        return;
    }

    const float  ratio    = processor.setTargetOutputSeconds (wanted);
    const double achieved = engine.getSourceSeconds() * (double) ratio;

    if (std::abs (achieved - wanted) > wanted * 0.02)
        say ("Capped at " + duration::format (achieved) + " - that is the "
             + juce::String (ratio, 0) + "x ceiling.", true);
    else
        say ("Stretched to " + duration::format (achieved), false);

    lengthEditor.giveAwayKeyboardFocus();
}

//==============================================================================

void ExtremeStretchEditor::paint (juce::Graphics& g)
{
    g.fillAll (ui::colours::bg);

    auto header = getLocalBounds().removeFromTop (52).reduced (16, 0);

    g.setColour (ui::colours::text);
    g.setFont (juce::FontOptions (22.0f).withStyle ("Bold"));
    g.drawText ("EXTREME STRETCH", header.removeFromLeft (240),
                juce::Justification::centredLeft);

    g.setColour (ui::colours::dim);
    g.setFont (juce::FontOptions (11.0f));
    g.drawText (sourceText, header, juce::Justification::centredRight);

    g.setColour (ui::colours::edge);
    g.fillRect (juce::Rectangle<int> (16, 51, getWidth() - 32, 1));

    // section captions — positions come from resized() so they cannot drift
    g.setColour (ui::colours::dim.withAlpha (0.75f));
    g.setFont (juce::FontOptions (9.5f).withStyle ("Bold"));
    g.drawText ("PAULSTRETCH", 20, paulstretchCaptionY, 240, 13, juce::Justification::centredLeft);
    g.drawText ("DESTRUCTION", 20, destructionCaptionY, 240, 13, juce::Justification::centredLeft);

    // "LENGTH" sits between the algorithm box and the entry field
    g.setColour (ui::colours::dim);
    g.setFont (juce::FontOptions (10.0f).withStyle ("Bold"));
    g.drawText ("LENGTH", lengthEditor.getX() - 58, lengthRow.getY(), 54, lengthRow.getHeight(),
                juce::Justification::centredRight);

    // and the source length it is derived from
    if (processor.getEngine().hasSource())
    {
        g.setColour (ui::colours::dim.withAlpha (0.8f));
        g.setFont (juce::FontOptions (11.0f));
        g.drawText ("from " + duration::format (processor.getEngine().getSourceSeconds())
                        + " of source",
                    lengthEditor.getRight() + 14, lengthRow.getY(),
                    getWidth() - lengthEditor.getRight() - 30, lengthRow.getHeight(),
                    juce::Justification::centredLeft);
    }

    // status strip
    auto footer = getLocalBounds().removeFromBottom (28).reduced (16, 4);

    g.setColour (messageText.isNotEmpty() ? (messageIsError ? ui::colours::hot : ui::colours::cold)
                                          : ui::colours::dim);
    g.setFont (juce::FontOptions (11.0f));
    g.drawText (messageText.isNotEmpty() ? messageText : statusText,
                footer, juce::Justification::centredLeft);
}

void ExtremeStretchEditor::resized()
{
    auto area = getLocalBounds().reduced (16, 0);

    area.removeFromTop (60);      // header
    area.removeFromBottom (28);   // status strip

    // transport row
    auto bar = area.removeFromTop (28);
    loadButton  .setBounds (bar.removeFromLeft (74));
    bar.removeFromLeft (6);
    playButton  .setBounds (bar.removeFromLeft (74));
    bar.removeFromLeft (6);
    loopButton  .setBounds (bar.removeFromLeft (66));
    bar.removeFromLeft (6);
    freezeButton.setBounds (bar.removeFromLeft (74));
    bar.removeFromLeft (6);
    liveButton  .setBounds (bar.removeFromLeft (74));
    renderButton.setBounds (bar.removeFromRight (84));

    area.removeFromTop (10);
    waveform.setBounds (area.removeFromTop (66));
    area.removeFromTop (8);

    // algorithm + target length
    lengthRow = area.removeFromTop (26);
    {
        auto row = lengthRow;
        algorithmBox.setBounds (row.removeFromLeft (130));
        row.removeFromLeft (18);
        row.removeFromLeft (54);                       // "LENGTH" caption, drawn in paint()
        lengthEditor.setBounds (row.removeFromLeft (190));
    }
    area.removeFromTop (10);

    auto layoutRow = [] (juce::Rectangle<int> row, std::vector<ui::Knob*> knobs)
    {
        if (knobs.empty())
            return;

        const int each = row.getWidth() / (int) knobs.size();

        for (auto* k : knobs)
            k->setBounds (row.removeFromLeft (each).reduced (6, 0));
    };

    // 13 caption + 100 knob, twice, with a gap
    auto knobArea = area.removeFromBottom (13 + 100 + 12 + 13 + 100);

    paulstretchCaptionY = knobArea.removeFromTop (13).getY();
    layoutRow (knobArea.removeFromTop (100), { &stretchKnob, &windowKnob, &shapeKnob,
                                               &pitchKnob, &widthKnob });

    knobArea.removeFromTop (12);

    destructionCaptionY = knobArea.removeFromTop (13).getY();
    layoutRow (knobArea.removeFromTop (100), { &blurKnob, &rotKnob, &scrambleKnob,
                                               &lowCutKnob, &highCutKnob, &gainKnob });

    spectrum.setBounds (area.withTrimmedBottom (10));
}

//==============================================================================

bool ExtremeStretchEditor::isInterestedInFileDrag (const juce::StringArray& files)
{
    for (const auto& f : files)
        if (processor.getEngine().getFormatManager().findFormatForFileExtension (
                juce::File (f).getFileExtension()) != nullptr)
            return true;

    return false;
}

void ExtremeStretchEditor::filesDropped (const juce::StringArray& files, int, int)
{
    for (const auto& f : files)
    {
        const juce::File file (f);

        if (processor.getEngine().getFormatManager().findFormatForFileExtension (file.getFileExtension()) != nullptr)
        {
            const auto error = processor.loadFile (file);
            say (error.isEmpty() ? "Loaded " + file.getFileName() : error, error.isNotEmpty());
            waveform.sourceChanged();
            return;
        }
    }
}

void ExtremeStretchEditor::chooseFile()
{
    chooser = std::make_unique<juce::FileChooser> (
        "Choose something to destroy",
        juce::File::getSpecialLocation (juce::File::userMusicDirectory),
        processor.getEngine().getFormatManager().getWildcardForAllFormats());

    chooser->launchAsync (juce::FileBrowserComponent::openMode
                              | juce::FileBrowserComponent::canSelectFiles,
                          [this] (const juce::FileChooser& fc)
                          {
                              const auto file = fc.getResult();

                              if (file == juce::File())
                                  return;

                              const auto error = processor.loadFile (file);
                              say (error.isEmpty() ? "Loaded " + file.getFileName() : error,
                                   error.isNotEmpty());
                              waveform.sourceChanged();
                          });
}

void ExtremeStretchEditor::startRender()
{
    if (renderJob != nullptr)
        return;

    if (! processor.getEngine().hasSource())
    {
        say ("Load a file before rendering.", true);
        return;
    }

    const auto suggested = processor.getLastFile()
                               .getParentDirectory()
                               .getChildFile (processor.getEngine().getSourceName()
                                                  .upToLastOccurrenceOf (".", false, false)
                                              + "-stretched.wav");

    chooser = std::make_unique<juce::FileChooser> ("Render the stretch to...", suggested, "*.wav");

    chooser->launchAsync (juce::FileBrowserComponent::saveMode
                              | juce::FileBrowserComponent::canSelectFiles
                              | juce::FileBrowserComponent::warnAboutOverwriting,
                          [this] (const juce::FileChooser& fc)
                          {
                              auto file = fc.getResult();

                              if (file == juce::File())
                                  return;

                              if (file.getFileExtension().isEmpty())
                                  file = file.withFileExtension ("wav");

                              auto* job = new RenderJob (processor.getEngine(), file,
                                                         processor.currentParams());

                              job->onFinished = [this] (juce::String result, juce::File written)
                              {
                                  say (result.isEmpty()
                                           ? "Rendered " + written.getFileName() + " ("
                                                 + juce::File::descriptionOfSizeInBytes (written.getSize()) + ")"
                                           : result,
                                       result.isNotEmpty());

                                  juce::MessageManager::callAsync ([this] { renderJob.reset(); });
                              };

                              renderJob.reset (job);
                              job->launchThread();
                          });
}

void ExtremeStretchEditor::say (const juce::String& message, bool isError)
{
    messageText    = message;
    messageIsError = isError;
    messageTicks   = 12 * 6;   // ~6 seconds at the editor's tick rate
    repaint();
}

//==============================================================================

void ExtremeStretchEditor::timerCallback()
{
    auto& engine = processor.getEngine();

    if (messageTicks > 0 && --messageTicks == 0)
    {
        messageText.clear();
        repaint();
    }

    const bool playing = engine.isPlaying();
    const bool live    = engine.getSourceMode() == StretchEngine::SourceMode::live;

    playButton.setButtonText (playing ? "STOP" : "PLAY");
    playButton.setToggleState (playing, juce::dontSendNotification);

    // Switching to live input should just start making noise.
    if (live && ! wasLive)
        engine.start();

    if (! live && wasLive)
        engine.stop();

    wasLive = live;

    const auto name = engine.getSourceName();

    if (name != sourceText && ! live)
    {
        sourceText = name.isEmpty() ? "no source" : name;
        waveform.sourceChanged();
    }

    if (live)
        sourceText = "live input";

    const auto algorithm = (Algorithm) juce::jlimit (0, numAlgorithms - 1,
                                                     algorithmBox.getSelectedItemIndex());

    if (algorithm != currentAlgorithm)
        algorithmChanged (algorithm);

    // The field doubles as the readout: it shows the length the current ratio
    // produces, until you click in and type a different one.
    if (! lengthEditor.hasKeyboardFocus (true))
    {
        const auto text = (live || ! engine.hasSource())
                              ? juce::String ("-")
                              : duration::format (engine.getTotalOutputSeconds());

        if (lengthEditor.getText() != text)
            lengthEditor.setText (text, juce::dontSendNotification);
    }

    juce::String status;

    if (live)
    {
        status << "live input   ";
    }
    else if (engine.hasSource())
    {
        status << duration::formatShort (engine.getOutputSeconds()) << "  /  "
               << duration::formatShort (engine.getTotalOutputSeconds()) << "     ";

        if (algorithm == Algorithm::onset)
            status << engine.getNumOnsets() << " attacks detected     ";
    }
    else
    {
        status << "drop a file, or switch on LIVE IN     ";
    }

    const auto underruns = engine.getUnderrunCount();

    if (underruns > 0)
        status << "  buffer misses " << underruns;

    if (status != statusText)
    {
        statusText = status;

        if (messageText.isEmpty())
            repaint();
    }
}

} // namespace es
