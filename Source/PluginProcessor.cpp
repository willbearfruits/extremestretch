#include "PluginProcessor.h"
#include "PluginEditor.h"

namespace es
{

namespace
{
    constexpr const char* lastFileProperty = "lastFile";

    juce::NormalisableRange<float> skewed (float lo, float hi, float centre)
    {
        juce::NormalisableRange<float> range (lo, hi);
        range.setSkewForCentre (centre);
        return range;
    }

    /** Logarithmic, but biased so the low end keeps most of the travel.
        STRETCH spans fifteen decades; a straight log map would squeeze 1x-100x
        — everything musically useful — into the first 13% of the knob. The
        exponent pulls that back out to roughly the first 40%, leaving the top
        of the sweep for the absurd end. */
    constexpr float stretchBias = 2.2f;

    juce::NormalisableRange<float> biasedLogRange (float lo, float hi, float bias)
    {
        return { lo, hi,
                 [bias] (float mn, float mx, float t)
                 {
                     const auto span = std::log (mx) - std::log (mn);
                     return std::exp (std::log (mn) + std::pow (juce::jmax (0.0f, t), bias) * span);
                 },
                 [bias] (float mn, float mx, float v)
                 {
                     const auto span = std::log (mx) - std::log (mn);
                     const auto t    = (std::log (juce::jlimit (mn, mx, v)) - std::log (mn)) / span;
                     return std::pow (juce::jmax (0.0f, t), 1.0f / bias);
                 },
                 [] (float mn, float mx, float v) { return juce::jlimit (mn, mx, v); } };
    }

    using FloatAttributes = juce::AudioParameterFloatAttributes;

    FloatAttributes shows (std::function<juce::String (float)> format)
    {
        return FloatAttributes().withStringFromValueFunction (
            [f = std::move (format)] (float value, int) { return f (value); });
    }

    juce::String asRatio (float v)
    {
        if (v < 10.0f)     return juce::String (v, 2) + " x";
        if (v < 10000.0f)  return juce::String (juce::roundToInt (v)) + " x";
        if (v < 1.0e6f)    return juce::String (v / 1.0e3f, 1) + "k x";
        if (v < 1.0e9f)    return juce::String (v / 1.0e6f, 1) + "M x";
        if (v < 1.0e12f)   return juce::String (v / 1.0e9f, 1) + "G x";
        return juce::String (v / 1.0e12f, 1) + "T x";
    }
    juce::String asPercent (float v) { return juce::String (juce::roundToInt (v * 100.0f)) + " %"; }
    juce::String asSemis (float v)   { return juce::String (v, 2) + " st"; }
    juce::String asDecibels (float v){ return juce::String (v, 1) + " dB"; }

    juce::String asSeconds (float v)
    {
        return v < 1.0f ? juce::String (juce::roundToInt (v * 1000.0f)) + " ms"
                        : juce::String (v, 2) + " s";
    }

    juce::String asHertz (float v)
    {
        return v >= 1000.0f ? juce::String (v / 1000.0f, 2) + " kHz"
                            : juce::String (juce::roundToInt (v)) + " Hz";
    }
}

juce::AudioProcessorValueTreeState::ParameterLayout ExtremeStretchProcessor::createParameterLayout()
{
    using Float = juce::AudioParameterFloat;
    using Bool  = juce::AudioParameterBool;

    std::vector<std::unique_ptr<juce::RangedAudioParameter>> p;

    // --- paulstretch proper -------------------------------------------------
    p.push_back (std::make_unique<Float> (juce::ParameterID { pid::stretch, 1 }, "Stretch",
                                          biasedLogRange (1.0f, 1.0e15f, stretchBias), 8.0f,
                                          shows (asRatio)));
    p.push_back (std::make_unique<Float> (juce::ParameterID { pid::window, 1 }, "Window",
                                          skewed (0.005f, 5.0f, 0.25f), 0.25f, shows (asSeconds)));
    p.push_back (std::make_unique<Bool>  (juce::ParameterID { pid::freeze, 1 }, "Freeze", false));

    juce::StringArray algorithmNames;

    for (int i = 0; i < numAlgorithms; ++i)
        algorithmNames.add (toString ((Algorithm) i));

    p.push_back (std::make_unique<juce::AudioParameterChoice> (
        juce::ParameterID { pid::algorithm, 1 }, "Algorithm", algorithmNames, 0));

    p.push_back (std::make_unique<Float> (juce::ParameterID { pid::shape, 1 }, "Shape",
                                          juce::NormalisableRange<float> (0.0f, 1.0f, 0.001f), 0.5f,
                                          shows (asPercent)));

    // --- extensions ---------------------------------------------------------
    p.push_back (std::make_unique<Float> (juce::ParameterID { pid::pitch, 1 }, "Pitch",
                                          juce::NormalisableRange<float> (-24.0f, 24.0f, 0.01f), 0.0f,
                                          shows (asSemis)));
    p.push_back (std::make_unique<Float> (juce::ParameterID { pid::blur, 1 }, "Blur",
                                          juce::NormalisableRange<float> (0.0f, 1.0f, 0.001f), 0.0f,
                                          shows (asPercent)));
    p.push_back (std::make_unique<Float> (juce::ParameterID { pid::rot, 1 }, "Rot",
                                          juce::NormalisableRange<float> (0.0f, 1.0f, 0.001f), 0.0f,
                                          shows (asPercent)));
    p.push_back (std::make_unique<Float> (juce::ParameterID { pid::scramble, 1 }, "Scramble",
                                          juce::NormalisableRange<float> (0.0f, 1.0f, 0.001f), 0.0f,
                                          shows (asPercent)));
    p.push_back (std::make_unique<Float> (juce::ParameterID { pid::lowCut, 1 }, "Low Cut",
                                          skewed (20.0f, 20000.0f, 1000.0f), 20.0f, shows (asHertz)));
    p.push_back (std::make_unique<Float> (juce::ParameterID { pid::highCut, 1 }, "High Cut",
                                          skewed (20.0f, 20000.0f, 1000.0f), 20000.0f, shows (asHertz)));
    p.push_back (std::make_unique<Float> (juce::ParameterID { pid::width, 1 }, "Width",
                                          juce::NormalisableRange<float> (0.0f, 1.0f, 0.001f), 1.0f,
                                          shows (asPercent)));

    // --- output -------------------------------------------------------------
    p.push_back (std::make_unique<Float> (juce::ParameterID { pid::gain, 1 }, "Gain",
                                          juce::NormalisableRange<float> (-60.0f, 12.0f, 0.1f), 0.0f,
                                          shows (asDecibels)));
    p.push_back (std::make_unique<Bool>  (juce::ParameterID { pid::loop, 1 }, "Loop", false));
    p.push_back (std::make_unique<Bool>  (juce::ParameterID { pid::live, 1 }, "Live Input", false));

    return { p.begin(), p.end() };
}

//==============================================================================

ExtremeStretchProcessor::ExtremeStretchProcessor()
    : juce::AudioProcessor (BusesProperties()
                                .withInput  ("Input",  juce::AudioChannelSet::stereo(), true)
                                .withOutput ("Output", juce::AudioChannelSet::stereo(), true)),
      apvts (*this, nullptr, "state", createParameterLayout())
{
    pStretch   = raw (pid::stretch);
    pWindow    = raw (pid::window);
    pAlgorithm = raw (pid::algorithm);
    pShape     = raw (pid::shape);
    pPitch    = raw (pid::pitch);
    pBlur     = raw (pid::blur);
    pRot      = raw (pid::rot);
    pScramble = raw (pid::scramble);
    pLowCut   = raw (pid::lowCut);
    pHighCut  = raw (pid::highCut);
    pWidth    = raw (pid::width);
    pFreeze   = raw (pid::freeze);
    pGain     = raw (pid::gain);
    pLoop     = raw (pid::loop);
    pLive     = raw (pid::live);

    // Read on the render thread, once per hop. Everything in here is a
    // lock-free atomic load.
    engine.setParameterProvider ([this] { return currentParams(); });
}

ExtremeStretchProcessor::~ExtremeStretchProcessor()
{
    engine.release();
}

std::atomic<float>* ExtremeStretchProcessor::raw (const char* id) const
{
    return apvts.getRawParameterValue (id);
}

StretchParams ExtremeStretchProcessor::currentParams() const
{
    StretchParams p;

    p.stretch        = pStretch  != nullptr ? pStretch->load()  : 8.0f;
    p.windowSeconds  = pWindow   != nullptr ? pWindow->load()   : 0.25f;
    p.pitchSemitones = pPitch    != nullptr ? pPitch->load()    : 0.0f;
    p.blur           = pBlur     != nullptr ? pBlur->load()     : 0.0f;
    p.rot            = pRot      != nullptr ? pRot->load()      : 0.0f;
    p.scramble       = pScramble != nullptr ? pScramble->load() : 0.0f;
    p.lowCutHz       = pLowCut   != nullptr ? pLowCut->load()   : 20.0f;
    p.highCutHz      = pHighCut  != nullptr ? pHighCut->load()  : 20000.0f;
    p.width          = pWidth    != nullptr ? pWidth->load()    : 1.0f;
    p.shape          = pShape    != nullptr ? pShape->load()    : 0.5f;
    p.freeze         = pFreeze   != nullptr && pFreeze->load() > 0.5f;

    const int index = pAlgorithm != nullptr ? (int) pAlgorithm->load() : 0;
    p.algorithm     = (Algorithm) juce::jlimit (0, numAlgorithms - 1, index);

    return p;
}

float ExtremeStretchProcessor::setTargetOutputSeconds (double seconds)
{
    const double sourceSeconds = engine.getSourceSeconds();

    if (sourceSeconds <= 0.0 || seconds <= 0.0)
        return pStretch != nullptr ? pStretch->load() : 8.0f;

    auto* parameter = apvts.getParameter (pid::stretch);

    if (parameter == nullptr)
        return 8.0f;

    const auto& range   = parameter->getNormalisableRange();
    const auto  wanted  = (float) (seconds / sourceSeconds);
    const auto  clamped = juce::jlimit (range.start, range.end, wanted);

    parameter->setValueNotifyingHost (range.convertTo0to1 (clamped));

    return clamped;
}

//==============================================================================

void ExtremeStretchProcessor::prepareToPlay (double sampleRate, int)
{
    engine.prepare (sampleRate, juce::jmax (1, getTotalNumOutputChannels()));

    outputGain.reset (sampleRate, 0.02);
    outputGain.setCurrentAndTargetValue (juce::Decibels::decibelsToGain (pGain->load()));
}

void ExtremeStretchProcessor::releaseResources()
{
    engine.release();
}

bool ExtremeStretchProcessor::isBusesLayoutSupported (const BusesLayout& layouts) const
{
    const auto& out = layouts.getMainOutputChannelSet();

    if (out != juce::AudioChannelSet::mono() && out != juce::AudioChannelSet::stereo())
        return false;

    const auto& in = layouts.getMainInputChannelSet();

    return in.isDisabled() || in == out;
}

void ExtremeStretchProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer&)
{
    juce::ScopedNoDenormals noDenormals;

    const int numIn  = getTotalNumInputChannels();
    const int numOut = getTotalNumOutputChannels();
    const int n      = buffer.getNumSamples();

    engine.setSourceMode (pLive != nullptr && pLive->load() > 0.5f
                              ? StretchEngine::SourceMode::live
                              : StretchEngine::SourceMode::file);

    engine.setLooping (pLoop != nullptr && pLoop->load() > 0.5f);

    if (numIn > 0 && engine.getSourceMode() == StretchEngine::SourceMode::live)
    {
        const juce::AudioBuffer<float> input (buffer.getArrayOfWritePointers(),
                                              juce::jmin (numIn, numOut), 0, n);
        engine.pushInput (input);
    }

    // Fills the whole buffer (and clears it first) — the stretched signal is a
    // generator, not an insert effect.
    engine.readOutput (buffer);

    outputGain.setTargetValue (juce::Decibels::decibelsToGain (pGain->load(), -60.0f));
    outputGain.applyGain (buffer, n);

    for (int ch = numOut; ch < buffer.getNumChannels(); ++ch)
        buffer.clear (ch, 0, n);
}

//==============================================================================

juce::String ExtremeStretchProcessor::loadFile (const juce::File& file)
{
    const auto error = engine.loadFile (file);

    if (error.isEmpty())
        apvts.state.setProperty (lastFileProperty, file.getFullPathName(), nullptr);

    return error;
}

juce::File ExtremeStretchProcessor::getLastFile() const
{
    const auto path = apvts.state.getProperty (lastFileProperty).toString();
    return path.isEmpty() ? juce::File() : juce::File (path);
}

void ExtremeStretchProcessor::getStateInformation (juce::MemoryBlock& destData)
{
    if (auto xml = apvts.copyState().createXml())
        copyXmlToBinary (*xml, destData);
}

void ExtremeStretchProcessor::setStateInformation (const void* data, int sizeInBytes)
{
    auto xml = getXmlFromBinary (data, sizeInBytes);

    if (xml == nullptr || ! xml->hasTagName (apvts.state.getType()))
        return;

    apvts.replaceState (juce::ValueTree::fromXml (*xml));

    const auto file = getLastFile();

    if (file.existsAsFile())
        engine.loadFile (file);
}

juce::AudioProcessorEditor* ExtremeStretchProcessor::createEditor()
{
    return new ExtremeStretchEditor (*this);
}

} // namespace es

//==============================================================================
juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new es::ExtremeStretchProcessor();
}
