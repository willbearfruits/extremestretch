#pragma once

#include <juce_dsp/juce_dsp.h>

#include <memory>
#include <vector>

namespace es
{

/** How a frame gets resynthesised. Only `smooth` is paulstretch proper; the
    rest are the variants and the extensions. */
enum class Algorithm
{
    smooth = 0,  ///< random phase — the classic. A cloud.
    onset,       ///< random phase, but attacks in the source are re-articulated coherently
    vocoder,     ///< phase-coherent. Tape slowed down rather than a cloud.
    grain,       ///< time domain, no FFT at all. Jittered grains.
    hold         ///< random phase over time-averaged magnitudes. Glassy and static.
};

inline constexpr int numAlgorithms = 5;

const char* toString (Algorithm) noexcept;

/** What the SHAPE control means depends on the algorithm. */
const char* shapeCaptionFor (Algorithm) noexcept;

/** Whether the spectral controls (blur/rot/scramble/cuts) do anything. */
bool isSpectral (Algorithm) noexcept;

/** A detected attack in the source, strength normalised to 0..1. */
struct Onset
{
    int   position = 0;
    float strength = 0.0f;
};

struct StretchParams
{
    // --- paulstretch proper -------------------------------------------------
    float     stretch       = 8.0f;      // 1 .. 1e15
    float     windowSeconds = 0.25f;     // 0.005 .. 5 s (snapped to a power of two)
    bool      freeze        = false;     // hold the read head still
    Algorithm algorithm     = Algorithm::smooth;
    float     shape         = 0.5f;      // meaning depends on the algorithm

    // --- extensions ---------------------------------------------------------
    float pitchSemitones = 0.0f;      // -24 .. +24
    float blur           = 0.0f;      // 0 .. 1  smear magnitudes across bins
    float rot            = 0.0f;      // 0 .. 1  progressive, *persistent* bin death
    float scramble       = 0.0f;      // 0 .. 1  local bin displacement
    float lowCutHz       = 20.0f;     // spectral high-pass
    float highCutHz      = 20000.0f;  // spectral low-pass
    float width          = 1.0f;      // 0 = channels share phase, 1 = fully decorrelated
};

/**
    Paul's Extreme Sound Stretch, plus four sibling algorithms.

    The algorithm is Nasca Octavian Paul's — author of Paul's Extreme Sound
    Stretch and ZynAddSubFX. This is an independent reimplementation written
    from the method, not a port of his code. The invention is his.

    The classic: take a long window of input, window it, FFT, throw the phases
    away and replace them with noise, IFFT, window again, and overlap-add at
    50%. The output hop is windowSize/2; the *input* hop is (windowSize/2) /
    stretch, which is the only place the stretch ratio appears.

    Windowing depends on how frames add:
      - random phase   -> frames add in POWER, so sum(w^2) must be flat.
                          Paul's sqrt(1 - |x|^2.5) ripples 2.16 dB and comes out
                          at unity gain with no normalisation.
      - coherent phase -> frames add in AMPLITUDE, so sum(w_a * w_s) must be flat.
                          sqrt(Hann) twice gives Hann, which is exactly 1.0 at
                          50% overlap. Used by the vocoder.
      - grains         -> windowed once only, so sum(w) must be flat. Hann.

    No allocation happens in renderHop(); setOrder() is the only sizing call.
    Not thread safe: one instance belongs to one render thread.
*/
class PaulStretch
{
public:
    static constexpr int minOrder = 8;   // 256 samples
    static constexpr int maxOrder = 18;  // 262144 samples (~5.9 s @ 44.1 kHz)

    static constexpr int displayBins = 160;

    PaulStretch() = default;

    void prepare (double sampleRate, int numChannels);
    void reset();

    /** Nearest power-of-two FFT order for a window length in seconds. */
    static int orderForSeconds (float seconds, double sampleRate) noexcept;

    void setOrder (int order);
    int  getOrder()       const noexcept { return fftOrder; }
    int  getWindowSize()  const noexcept { return windowSize; }
    int  getHopSize()     const noexcept { return windowSize / 2; }
    int  getNumBins()     const noexcept { return windowSize / 2 + 1; }
    int  getNumChannels() const noexcept { return numChannels; }

    /** Renders exactly getHopSize() samples per channel and advances the read
        head by hop/stretch. Reads from `source` with wrap-around, so a source
        shorter than the window is fine. `onsets` may be empty unless the
        algorithm is Algorithm::onset. */
    void renderHop (const juce::AudioBuffer<float>& source,
                    const std::vector<Onset>& onsets,
                    float* const* out,
                    int numOutChannels,
                    const StretchParams& p);

    /** Emits the half-window still held in the overlap-add tail. That tail has
        already been multiplied by the synthesis window, so it decays to exactly
        zero — this is what makes a window-size change silent instead of a click.
        Call it, push the samples, *then* setOrder(). */
    void flushTail (float* const* out, int numOutChannels);

    double getReadPosition() const noexcept { return readPos; }
    void   setReadPosition (double p) noexcept;

    /** Log-spaced peak magnitudes of the last frame, roughly in amplitude
        units. Written every hop, read by the UI — deliberately unsynchronised. */
    const std::vector<float>& getDisplaySpectrum() const noexcept { return display; }

private:
    void gatherFrame (const juce::AudioBuffer<float>& source, int sourceChannel,
                      int destChannel, const float* window);
    void shapeMagnitudes (int channel, const StretchParams& p);
    void updateRot (const StretchParams& p);
    void buildScramble (const StretchParams& p);
    void updateDisplaySpectrum();
    void renderGrainHop (const juce::AudioBuffer<float>& source, float* const* out,
                         int numOutChannels, const StretchParams& p);
    bool consumeOnset (const std::vector<Onset>& onsets, const StretchParams& p);
    void overlapAdd (float* const* out, int numOutChannels);

    double sampleRate  = 44100.0;
    int    numChannels = 2;
    int    fftOrder    = 0;
    int    windowSize  = 0;

    std::unique_ptr<juce::dsp::FFT> fft;

    std::vector<float> windowPaul, windowSqrtHann, windowHann;

    std::vector<std::vector<float>> fftData;    // [ch][2 * windowSize]
    std::vector<std::vector<float>> mags;       // [ch][numBins]  shaped
    std::vector<std::vector<float>> origMags;   // [ch][numBins]  before shaping
    std::vector<std::vector<float>> magTmp;     // [ch][numBins]
    std::vector<std::vector<float>> tail;       // [ch][hop]

    // vocoder state
    std::vector<std::vector<float>> prevPhase, sumPhase, binFreq;
    // hold state
    std::vector<std::vector<float>> heldMags;

    std::vector<float> health;    // [numBins]  persistent rot state
    std::vector<float> prefix;    // [numBins+1] blur prefix sums
    std::vector<int>   swapA, swapB;
    std::vector<float> display;   // [displayBins]

    juce::Random rng { 0x5f3759df };
    double    readPos           = 0.0;
    long long lastFrameStart    = 0;
    int       lastOnsetConsumed = -1;
    bool      rotActive         = false;
    bool      stateIsStale      = true;   // vocoder/hold state needs re-seeding

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (PaulStretch)
};

} // namespace es
