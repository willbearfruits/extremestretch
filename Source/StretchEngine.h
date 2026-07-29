#pragma once

#include "PaulStretch.h"

#include <juce_audio_formats/juce_audio_formats.h>

#include <atomic>
#include <functional>

namespace es
{

/**
    Owns a PaulStretch, a source, a render thread and a lock-free hand-off to
    the audio thread.

    A single FFT can be 262144 points — tens of milliseconds of work — so the
    stretching cannot happen in the audio callback. Instead a background thread
    keeps a FIFO topped up and processBlock() only ever memcpys out of it. That
    also means playback starts instantly and stays memory-bounded no matter how
    absurd the stretch ratio is: a 60 s file at 1000x is 16 hours of audio that
    is never materialised anywhere.

    The FIFO is guarded by a SpinLock which the audio thread only ever *tries*
    to take — on the rare miss (the producer is mid-write or resetting) it emits
    one block of silence instead of blocking.
*/
class StretchEngine : private juce::Thread
{
public:
    enum class SourceMode
    {
        file,  ///< stretch a loaded file
        live   ///< stretch whatever is arriving at the plugin input
    };

    static constexpr double captureSeconds = 20.0;

    StretchEngine();
    ~StretchEngine() override;

    void prepare (double sampleRate, int numChannels);
    void release();

    //== source ================================================================
    /** Returns an empty string on success, otherwise a human-readable error. */
    juce::String loadFile (const juce::File& file);
    void clearSource();

    bool         hasSource()        const noexcept { return sourceSamples.load() > 0; }
    juce::String getSourceName()    const;
    double       getSourceSeconds() const noexcept;

    /** For the waveform display. Only touch from the message thread. */
    const juce::AudioBuffer<float>& getSourceBufferForDisplay() const noexcept { return fileBuffer; }

    void       setSourceMode (SourceMode mode);
    SourceMode getSourceMode() const noexcept { return sourceMode.load(); }

    //== transport =============================================================
    void start();
    void stop();
    void rewind();

    /** Jumps to a fraction of the way through the source. Takes effect on the
        next render, keeping the overlap-add tail so the splice crossfades. */
    void seekToSourceNormalised (double position);

    bool isPlaying()  const noexcept { return playing.load(); }
    bool isFinished() const noexcept { return producerDone.load() && ! playing.load(); }

    void setLooping (bool shouldLoop) noexcept { looping.store (shouldLoop); }
    bool isLooping() const noexcept            { return looping.load(); }

    double getPlaybackProgress()   const noexcept;  ///< 0..1 through the source
    double getOutputSeconds()      const noexcept;  ///< heard so far
    double getTotalOutputSeconds() const noexcept;  ///< length of the whole stretched render
    int    getUnderrunCount()      const noexcept { return underruns.load(); }

    /** Attacks detected in the source, used by Algorithm::onset. */
    int getNumOnsets() const noexcept { return numOnsets.load(); }

    //== parameters ============================================================
    /** Called on the render thread once per hop. Read atomics in here, do not
        allocate and do not lock against the audio thread. */
    void setParameterProvider (std::function<StretchParams()> provider);

    //== audio thread ==========================================================
    void pushInput (const juce::AudioBuffer<float>& input);

    /** Fills `output` and returns how many samples were actually delivered.
        Anything short of output.getNumSamples() is an underrun and the rest of
        the buffer is silence. */
    int readOutput (juce::AudioBuffer<float>& output);

    //== display ===============================================================
    void copyDisplaySpectrum (float* dest, int numValues) const;

    //== offline ===============================================================
    /** Renders the stretched source straight to a 24-bit WAV. Runs on the
        calling thread with its own PaulStretch, so live playback is untouched.
        `onProgress` returns false to cancel. */
    juce::String renderToFile (const juce::File& destination,
                               double maxSeconds,
                               const StretchParams& params,
                               std::function<bool (double)> onProgress);

    juce::AudioFormatManager& getFormatManager() noexcept { return formats; }

private:
    void run() override;
    bool produce();
    void pushHopToFifo (int numSamples);
    void rebuildFileBuffer();
    void computeOnsets();
    double sourcePositionInSamples() const noexcept;
    StretchParams fetchParams() const;

    /** How far ahead the render thread is allowed to run. Deliberately small:
        the FIFO is sized for the largest possible hop, and filling all of it
        would mean a knob turn taking ten seconds to be heard. */
    int targetBufferedSamples (int hop) const noexcept
    {
        return juce::jmax (hop * 3, (int) (0.2 * sampleRate));
    }

    static constexpr int maxHop      = 1 << (PaulStretch::maxOrder - 1);
    static constexpr int fifoCapacity = maxHop * 3 + 1;

    juce::AudioFormatManager formats;

    PaulStretch stretcher;
    double      sampleRate  = 44100.0;
    int         numChannels = 2;

    // Source. `rawBuffer` is whatever came off disk; `fileBuffer` is that
    // resampled to the engine rate, which is what actually gets read.
    juce::CriticalSection    sourceLock;
    juce::AudioBuffer<float> rawBuffer, fileBuffer, captureBuffer;
    std::vector<Onset>       onsets;
    double                   rawSampleRate = 0.0;
    juce::String             sourceName;
    std::atomic<int>         sourceSamples { 0 };
    std::atomic<int>         numOnsets { 0 };
    std::atomic<SourceMode>  sourceMode { SourceMode::file };
    std::atomic<long long>   captureWrite { 0 };
    std::atomic<double>      pendingSeek { -1.0 };
    std::atomic<double>      readPosSnapshot { 0.0 };

    // Hand-off.
    juce::SpinLock           fifoLock;
    juce::AbstractFifo       fifo { fifoCapacity };
    juce::AudioBuffer<float> fifoBuffer, staging;

    std::atomic<bool> playing      { false };
    std::atomic<bool> looping      { false };
    std::atomic<bool> resetPending { true };
    std::atomic<bool> producerDone { false };
    std::atomic<int>  underruns    { 0 };
    std::atomic<long long> samplesConsumed { 0 };
    std::atomic<double>    lastStretch     { 8.0 };

    mutable juce::CriticalSection  providerLock;
    std::function<StretchParams()> paramProvider;

    mutable juce::SpinLock spectrumLock;
    std::vector<float>     spectrumSnapshot;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (StretchEngine)
};

} // namespace es
