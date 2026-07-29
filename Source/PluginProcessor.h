#pragma once

#include "StretchEngine.h"

#include <juce_audio_processors/juce_audio_processors.h>

namespace es
{

namespace pid
{
    inline constexpr const char* stretch   = "stretch";
    inline constexpr const char* window    = "window";
    inline constexpr const char* algorithm = "algorithm";
    inline constexpr const char* shape     = "shape";
    inline constexpr const char* pitch    = "pitch";
    inline constexpr const char* blur     = "blur";
    inline constexpr const char* rot      = "rot";
    inline constexpr const char* scramble = "scramble";
    inline constexpr const char* lowCut   = "lowcut";
    inline constexpr const char* highCut  = "highcut";
    inline constexpr const char* width    = "width";
    inline constexpr const char* freeze   = "freeze";
    inline constexpr const char* gain     = "gain";
    inline constexpr const char* loop     = "loop";
    inline constexpr const char* live     = "live";
}

class ExtremeStretchProcessor final : public juce::AudioProcessor
{
public:
    ExtremeStretchProcessor();
    ~ExtremeStretchProcessor() override;

    void prepareToPlay (double sampleRate, int maximumExpectedSamplesPerBlock) override;
    void releaseResources() override;
    bool isBusesLayoutSupported (const BusesLayout&) const override;
    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override                        { return true; }

    const juce::String getName() const override            { return "Extreme Stretch"; }
    bool acceptsMidi() const override                      { return false; }
    bool producesMidi() const override                     { return false; }
    bool isMidiEffect() const override                     { return false; }
    double getTailLengthSeconds() const override           { return 0.0; }

    int getNumPrograms() override                          { return 1; }
    int getCurrentProgram() override                       { return 0; }
    void setCurrentProgram (int) override                  {}
    const juce::String getProgramName (int) override       { return "Default"; }
    void changeProgramName (int, const juce::String&) override {}

    void getStateInformation (juce::MemoryBlock&) override;
    void setStateInformation (const void*, int) override;

    //== our own ===============================================================
    StretchEngine& getEngine() noexcept { return engine; }

    /** Snapshot of every parameter, taken on the render thread. */
    StretchParams currentParams() const;

    /** Loads a file and remembers the path in the plugin state. */
    juce::String loadFile (const juce::File&);
    juce::File   getLastFile() const;

    /** Sets STRETCH so the whole source lasts `seconds`. Returns the ratio
        actually applied, which is clamped to the parameter range. */
    float setTargetOutputSeconds (double seconds);

    static juce::AudioProcessorValueTreeState::ParameterLayout createParameterLayout();

    juce::AudioProcessorValueTreeState apvts;

private:
    std::atomic<float>* raw (const char* id) const;

    StretchEngine engine;

    std::atomic<float>* pStretch   = nullptr;
    std::atomic<float>* pWindow    = nullptr;
    std::atomic<float>* pAlgorithm = nullptr;
    std::atomic<float>* pShape     = nullptr;
    std::atomic<float>* pPitch    = nullptr;
    std::atomic<float>* pBlur     = nullptr;
    std::atomic<float>* pRot      = nullptr;
    std::atomic<float>* pScramble = nullptr;
    std::atomic<float>* pLowCut   = nullptr;
    std::atomic<float>* pHighCut  = nullptr;
    std::atomic<float>* pWidth    = nullptr;
    std::atomic<float>* pFreeze   = nullptr;
    std::atomic<float>* pGain     = nullptr;
    std::atomic<float>* pLoop     = nullptr;
    std::atomic<float>* pLive     = nullptr;

    juce::SmoothedValue<float> outputGain;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ExtremeStretchProcessor)
};

} // namespace es
