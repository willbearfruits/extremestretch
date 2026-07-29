/*
    Extreme Stretch self-test.

    Links the same PaulStretch.cpp / StretchEngine.cpp the plugin runs, so this
    is a check on the shipping DSP rather than on a copy of it. Every assertion
    has to pass; the binary exits non-zero otherwise.

        ./build/ExtremeStretchSelfTest_artefacts/Release/extremestretch-selftest
*/

#include "../Source/StretchEngine.h"
#include "../Source/Duration.h"

#include <juce_events/juce_events.h>

#include <cmath>
#include <cstdio>

namespace
{

int checksRun    = 0;
int checksFailed = 0;

void check (bool ok, juce::String name, juce::String detail = {})
{
    ++checksRun;

    if (! ok)
        ++checksFailed;

    juce::String line;
    line << (ok ? "  pass   " : "  FAIL   ") << name;

    if (detail.isNotEmpty())
        line << "   (" << detail << ")";

    std::puts (line.toRawUTF8());
}

void section (const char* title)
{
    std::printf ("\n-- %s\n", title);
}

//==============================================================================
// helpers

double rmsOf (const juce::AudioBuffer<float>& b, int channel, int start, int num)
{
    num = juce::jmin (num, b.getNumSamples() - start);

    if (num <= 0)
        return 0.0;

    double sum = 0.0;
    const auto* d = b.getReadPointer (channel);

    for (int i = 0; i < num; ++i)
        sum += (double) d[start + i] * d[start + i];

    return std::sqrt (sum / num);
}

double rmsOf (const juce::AudioBuffer<float>& b)
{
    return rmsOf (b, 0, 0, b.getNumSamples());
}

bool allFinite (const juce::AudioBuffer<float>& b)
{
    for (int ch = 0; ch < b.getNumChannels(); ++ch)
    {
        const auto* d = b.getReadPointer (ch);

        for (int i = 0; i < b.getNumSamples(); ++i)
            if (! std::isfinite (d[i]))
                return false;
    }

    return true;
}

double peakOf (const juce::AudioBuffer<float>& b)
{
    return (double) b.getMagnitude (0, b.getNumSamples());
}

/** Peak frequency of a buffer, with parabolic interpolation across the peak. */
double dominantFrequency (const juce::AudioBuffer<float>& b, double sampleRate,
                          int channel = 0, int startSample = 0)
{
    constexpr int order = 15;                 // 32768
    constexpr int size  = 1 << order;

    if (b.getNumSamples() - startSample < size)
        return 0.0;

    std::vector<float> data ((size_t) size * 2, 0.0f);
    const auto* src = b.getReadPointer (channel) + startSample;

    for (int i = 0; i < size; ++i)
    {
        const auto w = 0.5f - 0.5f * std::cos (juce::MathConstants<float>::twoPi * (float) i / (float) (size - 1));
        data[(size_t) i] = src[i] * w;
    }

    juce::dsp::FFT fft (order);
    fft.performFrequencyOnlyForwardTransform (data.data());

    int best = 1;

    for (int k = 2; k < size / 2; ++k)
        if (data[(size_t) k] > data[(size_t) best])
            best = k;

    // parabolic refinement
    const double a = data[(size_t) best - 1];
    const double m = data[(size_t) best];
    const double c = data[(size_t) best + 1];
    const double denom = a - 2.0 * m + c;
    const double delta = std::abs (denom) > 1.0e-12 ? 0.5 * (a - c) / denom : 0.0;

    return ((double) best + delta) * sampleRate / (double) size;
}

double correlation (const juce::AudioBuffer<float>& b, int start, int num)
{
    if (b.getNumChannels() < 2)
        return 1.0;

    num = juce::jmin (num, b.getNumSamples() - start);

    const auto* l = b.getReadPointer (0) + start;
    const auto* r = b.getReadPointer (1) + start;

    double dot = 0.0, ll = 0.0, rr = 0.0;

    for (int i = 0; i < num; ++i)
    {
        dot += (double) l[i] * r[i];
        ll  += (double) l[i] * l[i];
        rr  += (double) r[i] * r[i];
    }

    const double d = std::sqrt (ll * rr);
    return d > 1.0e-15 ? dot / d : 0.0;
}

juce::AudioBuffer<float> makeSine (double freq, double sampleRate, double seconds,
                                   float amplitude = 0.5f, int channels = 2)
{
    juce::AudioBuffer<float> b (channels, (int) (sampleRate * seconds));

    for (int ch = 0; ch < channels; ++ch)
        for (int i = 0; i < b.getNumSamples(); ++i)
            b.setSample (ch, i, amplitude * std::sin (juce::MathConstants<double>::twoPi * freq * i / sampleRate));

    return b;
}

juce::AudioBuffer<float> makeNoise (double sampleRate, double seconds,
                                    float rmsTarget = 0.25f, int channels = 2)
{
    juce::AudioBuffer<float> b (channels, (int) (sampleRate * seconds));
    juce::Random rng (12345);

    for (int ch = 0; ch < channels; ++ch)
        for (int i = 0; i < b.getNumSamples(); ++i)
            b.setSample (ch, i, rmsTarget * std::sqrt (3.0f) * (rng.nextFloat() * 2.0f - 1.0f));

    return b;
}

struct RenderResult
{
    juce::AudioBuffer<float> audio;
    double                   readPosition = 0.0;
    int                      hopSize      = 0;
};

RenderResult renderHops (const juce::AudioBuffer<float>& source,
                         const es::StretchParams& params,
                         double sampleRate, int numChannels, int numHops,
                         const std::vector<es::Onset>& onsets = {})
{
    es::PaulStretch stretcher;
    stretcher.prepare (sampleRate, numChannels);
    stretcher.setOrder (es::PaulStretch::orderForSeconds (params.windowSeconds, sampleRate));

    const int hop = stretcher.getHopSize();

    RenderResult result;
    result.hopSize = hop;
    result.audio.setSize (numChannels, hop * numHops);
    result.audio.clear();

    juce::AudioBuffer<float> block (numChannels, hop);

    for (int i = 0; i < numHops; ++i)
    {
        stretcher.renderHop (source, onsets, block.getArrayOfWritePointers(), numChannels, params);

        for (int ch = 0; ch < numChannels; ++ch)
            result.audio.copyFrom (ch, i * hop, block, ch, 0, hop);
    }

    result.readPosition = stretcher.getReadPosition();
    return result;
}

/** Normalised correlation between two mono spans. */
double correlate (const float* a, const float* b, int num)
{
    double dot = 0.0, aa = 0.0, bb = 0.0;

    for (int i = 0; i < num; ++i)
    {
        dot += (double) a[i] * b[i];
        aa  += (double) a[i] * a[i];
        bb  += (double) b[i] * b[i];
    }

    const double d = std::sqrt (aa * bb);
    return d > 1.0e-15 ? dot / d : 0.0;
}

juce::AudioBuffer<float> makeClickTrain (double sampleRate, double seconds, double clicksPerSecond,
                                         std::vector<int>* positionsOut = nullptr)
{
    juce::AudioBuffer<float> b (2, (int) (sampleRate * seconds));
    b.clear();

    const int spacing = (int) (sampleRate / clicksPerSecond);
    const int tail    = juce::jmin (400, spacing - 1);

    if (positionsOut != nullptr)
        positionsOut->clear();

    for (int start = spacing / 2; start < b.getNumSamples() - tail; start += spacing)
    {
        if (positionsOut != nullptr)
            positionsOut->push_back (start);

        for (int ch = 0; ch < 2; ++ch)
            for (int i = 0; i < tail; ++i)
            {
                const auto env = (float) std::exp (-12.0 * i / sampleRate * 40.0);
                b.setSample (ch, start + i,
                             0.7f * env * std::sin (juce::MathConstants<float>::twoPi * 900.0f
                                                        * (float) i / (float) sampleRate));
            }
    }

    return b;
}

//==============================================================================

void testFftAssumptions()
{
    section ("FFT contract");

    // Everything downstream assumes JUCE's real-only pair round-trips to unity
    // and that only bins 0..N/2 need filling before the inverse.
    constexpr int order = 10;
    constexpr int size  = 1 << order;

    std::vector<float> data ((size_t) size * 2, 0.0f);
    std::vector<float> original ((size_t) size, 0.0f);

    for (int i = 0; i < size; ++i)
    {
        const auto v = (float) (0.4 * std::sin (juce::MathConstants<double>::twoPi * 7.0 * i / size)
                              + 0.2 * std::cos (juce::MathConstants<double>::twoPi * 31.0 * i / size));
        data[(size_t) i]     = v;
        original[(size_t) i] = v;
    }

    juce::dsp::FFT fft (order);
    fft.performRealOnlyForwardTransform (data.data(), true);
    fft.performRealOnlyInverseTransform (data.data());

    double worst = 0.0;

    for (int i = 0; i < size; ++i)
        worst = juce::jmax (worst, std::abs ((double) data[(size_t) i] - original[(size_t) i]));

    check (worst < 1.0e-4, "forward/inverse round-trips at unity gain",
           "max error " + juce::String (worst, 9));
}

void testWindowMath()
{
    section ("window");

    // Random phases make overlapping frames add in power, so what matters is
    // sum(w^2) across the 50% overlap, not sum(w).
    constexpr int n   = 4096;
    const int     hop = n / 2;

    std::vector<double> w ((size_t) n);

    for (int i = 0; i < n; ++i)
    {
        const double x = 2.0 * i / (double) (n - 1) - 1.0;
        w[(size_t) i] = std::sqrt (juce::jmax (0.0, 1.0 - std::pow (std::abs (x), 2.5)));
    }

    double lo = 1.0e9, hi = -1.0e9, mean = 0.0;

    for (int i = 0; i < hop; ++i)
    {
        const double s = w[(size_t) i] * w[(size_t) i]
                       + w[(size_t) (i + hop)] * w[(size_t) (i + hop)];
        lo = juce::jmin (lo, s);
        hi = juce::jmax (hi, s);
        mean += s / hop;
    }

    check (hi / lo < 1.75, "power overlap ripple stays under 2.5 dB",
           juce::String (10.0 * std::log10 (hi / lo), 2) + " dB");
    check (std::abs (mean - 1.4286) < 0.02, "mean overlap power matches 2 - 2/3.5",
           juce::String (mean, 4));
    check (w.front() < 1.0e-9 && w.back() < 1.0e-9, "window starts and ends at zero");
}

void testUnityGain()
{
    section ("level");

    constexpr double sr = 44100.0;

    auto source = makeNoise (sr, 4.0, 0.25f);

    es::StretchParams p;
    p.stretch       = 20.0f;
    p.windowSeconds = 0.25f;

    auto r = renderHops (source, p, sr, 2, 10);

    // Skip the first two hops: the overlap-add tail has to fill first.
    const auto measured = rmsOf (r.audio, 0, r.hopSize * 2, r.hopSize * 6);
    const auto db       = 20.0 * std::log10 (measured / 0.25);

    check (std::abs (db) < 1.5, "white noise comes out at the level it went in",
           juce::String (db, 2) + " dB");
    check (allFinite (r.audio), "no NaN or Inf");
    check (peakOf (r.audio) < 4.0, "output stays bounded",
           "peak " + juce::String (peakOf (r.audio), 3));
}

void testPitchIsPreserved()
{
    section ("frequency");

    constexpr double sr = 44100.0;

    auto source = makeSine (1000.0, sr, 2.0);

    es::StretchParams p;
    p.stretch       = 50.0f;
    p.windowSeconds = 0.25f;

    auto r  = renderHops (source, p, sr, 2, 12);
    auto f0 = dominantFrequency (r.audio, sr, 0, r.hopSize * 2);

    check (std::abs (f0 - 1000.0) < 15.0, "a stretched 1 kHz sine is still 1 kHz",
           juce::String (f0, 2) + " Hz");

    p.pitchSemitones = 12.0f;
    auto up  = renderHops (source, p, sr, 2, 12);
    auto f1  = dominantFrequency (up.audio, sr, 0, up.hopSize * 2);

    check (std::abs (f1 - 2000.0) < 30.0, "+12 semitones lands on the octave",
           juce::String (f1, 2) + " Hz");

    p.pitchSemitones = -12.0f;
    auto down = renderHops (source, p, sr, 2, 12);
    auto f2   = dominantFrequency (down.audio, sr, 0, down.hopSize * 2);

    check (std::abs (f2 - 500.0) < 15.0, "-12 semitones lands an octave down",
           juce::String (f2, 2) + " Hz");
}

void testTimeBase()
{
    section ("time base");

    constexpr double sr = 44100.0;

    auto source = makeNoise (sr, 8.0);

    es::StretchParams p;
    p.stretch       = 32.0f;
    p.windowSeconds = 0.1f;

    constexpr int hops = 20;
    auto r = renderHops (source, p, sr, 2, hops);

    const double expected = (double) (r.hopSize * hops) / 32.0;
    const double error    = std::abs (r.readPosition - expected) / expected;

    check (error < 1.0e-9, "input advances at exactly output/stretch",
           juce::String (r.readPosition, 1) + " vs " + juce::String (expected, 1));

    p.freeze = true;
    auto frozen = renderHops (source, p, sr, 2, 8);

    check (frozen.readPosition == 0.0, "freeze pins the read head");
    check (rmsOf (frozen.audio, 0, frozen.hopSize * 2, frozen.hopSize * 4) > 0.01,
           "freeze still produces sound");
}

void testDestruction()
{
    section ("destruction");

    constexpr double sr = 44100.0;

    auto source = makeNoise (sr, 4.0);

    es::StretchParams p;
    p.windowSeconds = 0.05f;
    p.stretch       = 20.0f;

    // rot has to be persistent: the drone should keep eroding frame after frame.
    es::PaulStretch stretcher;
    stretcher.prepare (sr, 2);
    stretcher.setOrder (es::PaulStretch::orderForSeconds (p.windowSeconds, sr));

    const int hop = stretcher.getHopSize();
    juce::AudioBuffer<float> block (2, hop);

    auto renderInto = [&] (const es::StretchParams& params, int count)
    {
        double last = 0.0;

        for (int i = 0; i < count; ++i)
        {
            stretcher.renderHop (source, {}, block.getArrayOfWritePointers(), 2, params);
            last = rmsOf (block);
        }

        return last;
    };

    p.rot = 0.0f;
    const auto clean = renderInto (p, 6);

    p.rot = 1.0f;
    const auto rotted = renderInto (p, 60);

    check (rotted < clean * 0.5, "rot erodes the spectrum over time",
           juce::String (20.0 * std::log10 (juce::jmax (1.0e-9, rotted / clean)), 1) + " dB");

    p.rot = 0.0f;
    const auto healed = renderInto (p, 250);

    check (healed > clean * 0.7, "backing rot off heals the damage",
           juce::String (20.0 * std::log10 (juce::jmax (1.0e-9, healed / clean)), 1) + " dB");

    // blur keeps its level rather than quietly dropping it
    es::StretchParams b;
    b.windowSeconds = 0.1f;
    b.stretch       = 10.0f;

    auto flat    = renderHops (source, b, sr, 2, 8);
    b.blur       = 0.6f;
    auto blurred = renderHops (source, b, sr, 2, 8);

    const auto ratio = rmsOf (blurred.audio, 0, blurred.hopSize * 2, blurred.hopSize * 4)
                     / juce::jmax (1.0e-9, rmsOf (flat.audio, 0, flat.hopSize * 2, flat.hopSize * 4));

    check (std::abs (20.0 * std::log10 (ratio)) < 2.0, "blur is level-compensated",
           juce::String (20.0 * std::log10 (ratio), 2) + " dB");

    // scramble should change the sound without changing the level much
    es::StretchParams s = b;
    s.blur     = 0.0f;
    s.scramble = 0.8f;
    auto scrambled = renderHops (source, s, sr, 2, 8);

    check (allFinite (scrambled.audio), "scramble stays finite");
    check (rmsOf (scrambled.audio, 0, scrambled.hopSize * 2, scrambled.hopSize * 4) > 0.05,
           "scramble still produces sound");
}

void testSpectralFilter()
{
    section ("spectral filter");

    constexpr double sr = 44100.0;

    auto source = makeSine (1000.0, sr, 2.0);

    es::StretchParams p;
    p.stretch       = 20.0f;
    p.windowSeconds = 0.1f;

    auto open = renderHops (source, p, sr, 2, 8);

    p.lowCutHz = 8000.0f;
    auto highPassed = renderHops (source, p, sr, 2, 8);

    p.lowCutHz  = 20.0f;
    p.highCutHz = 200.0f;
    auto lowPassed = renderHops (source, p, sr, 2, 8);

    const auto reference = rmsOf (open.audio, 0, open.hopSize * 2, open.hopSize * 4);

    check (rmsOf (highPassed.audio, 0, highPassed.hopSize * 2, highPassed.hopSize * 4) < reference * 0.05,
           "an 8 kHz low cut removes a 1 kHz tone");
    check (rmsOf (lowPassed.audio, 0, lowPassed.hopSize * 2, lowPassed.hopSize * 4) < reference * 0.05,
           "a 200 Hz high cut removes a 1 kHz tone");
    check (reference > 0.05, "the unfiltered reference is actually audible",
           juce::String (reference, 4));
}

void testStereo()
{
    section ("stereo");

    constexpr double sr = 44100.0;

    es::StretchParams p;
    p.stretch       = 20.0f;
    p.windowSeconds = 0.1f;

    // A source whose channels are identical: sharing the phase must then give
    // back two identical channels.
    auto correlated = makeNoise (sr, 4.0);
    correlated.copyFrom (1, 0, correlated, 0, 0, correlated.getNumSamples());

    p.width = 0.0f;
    auto narrow = renderHops (correlated, p, sr, 2, 8);

    p.width = 1.0f;
    auto wide = renderHops (correlated, p, sr, 2, 8);

    const auto cNarrow = correlation (narrow.audio, narrow.hopSize * 2, narrow.hopSize * 4);
    const auto cWide   = correlation (wide.audio,   wide.hopSize * 2,   wide.hopSize * 4);

    check (cNarrow > 0.999, "width 0 keeps the channels phase-locked",
           juce::String (cNarrow, 5));
    check (std::abs (cWide) < 0.3, "width 1 fully decorrelates them",
           juce::String (cWide, 4));

    // With *independent* channels, width 0 still shares the phase but the
    // magnitudes differ. For independent Rayleigh magnitudes the correlation
    // works out to E|L|E|R| / sqrt(E|L|^2 E|R|^2) = pi/4 — hitting that number
    // is a sharp check that the phases really are shared per bin.
    auto independent = makeNoise (sr, 4.0);

    p.width = 0.0f;
    auto mixed = renderHops (independent, p, sr, 2, 8);

    const auto cMixed = correlation (mixed.audio, mixed.hopSize * 2, mixed.hopSize * 4);

    check (std::abs (cMixed - juce::MathConstants<double>::pi / 4.0) < 0.04,
           "shared phase over independent magnitudes lands on pi/4",
           juce::String (cMixed, 4) + " vs " + juce::String (juce::MathConstants<double>::pi / 4.0, 4));
}

void testMonoSource()
{
    section ("channel handling");

    constexpr double sr = 44100.0;

    auto mono = makeSine (440.0, sr, 2.0, 0.4f, 1);

    es::StretchParams p;
    p.stretch       = 16.0f;
    p.windowSeconds = 0.1f;

    auto r = renderHops (mono, p, sr, 2, 8);

    check (rmsOf (r.audio, 0, r.hopSize * 2, r.hopSize * 4) > 0.05, "mono source fills the left channel");
    check (rmsOf (r.audio, 1, r.hopSize * 2, r.hopSize * 4) > 0.05, "mono source fills the right channel");

    // a source shorter than the analysis window has to wrap, not read garbage
    auto tiny = makeSine (440.0, sr, 0.01, 0.4f, 2);
    auto wrapped = renderHops (tiny, p, sr, 2, 4);

    check (allFinite (wrapped.audio), "a source shorter than the window still renders");
    check (rmsOf (wrapped.audio, 0, wrapped.hopSize * 2, wrapped.hopSize * 2) > 0.01,
           "the short source wraps rather than going silent");
}

void testDeclick()
{
    section ("declick");

    constexpr double sr = 44100.0;

    auto source = makeNoise (sr, 4.0);

    es::StretchParams p;
    p.stretch       = 20.0f;
    p.windowSeconds = 0.1f;

    es::PaulStretch stretcher;
    stretcher.prepare (sr, 2);
    stretcher.setOrder (es::PaulStretch::orderForSeconds (p.windowSeconds, sr));

    const int hop = stretcher.getHopSize();
    juce::AudioBuffer<float> block (2, hop);

    for (int i = 0; i < 6; ++i)
        stretcher.renderHop (source, {}, block.getArrayOfWritePointers(), 2, p);

    stretcher.flushTail (block.getArrayOfWritePointers(), 2);

    // The tail has already been multiplied by the synthesis window, so it has
    // to arrive at zero — that is the whole reason a window-size change can be
    // silent instead of a step.
    const auto lastSample = std::abs (block.getSample (0, hop - 1));
    const auto tailRms    = rmsOf (block, 0, 0, hop);

    check (lastSample < 1.0e-6, "the flushed tail ends at zero",
           juce::String (lastSample, 9));
    check (tailRms > 0.001, "the flushed tail is not simply empty",
           juce::String (tailRms, 5));

    // hop boundaries should not be discontinuities
    auto r = renderHops (source, p, sr, 2, 8);
    const auto* d = r.audio.getReadPointer (0);

    double sumDelta = 0.0;
    int    count    = 0;

    for (int i = r.hopSize * 2 + 1; i < r.hopSize * 6; ++i)
    {
        sumDelta += std::abs ((double) d[i] - d[i - 1]);
        ++count;
    }

    const double meanDelta = sumDelta / juce::jmax (1, count);
    double worstBoundary = 0.0;

    for (int h = 3; h < 6; ++h)
    {
        const int i = h * r.hopSize;
        worstBoundary = juce::jmax (worstBoundary, std::abs ((double) d[i] - d[i - 1]));
    }

    check (worstBoundary < meanDelta * 8.0, "no step discontinuity at hop boundaries",
           juce::String (worstBoundary / juce::jmax (1.0e-12, meanDelta), 2) + "x mean");
}

void testExtremeSettings()
{
    section ("extremes");

    constexpr double sr = 48000.0;

    auto source = makeNoise (sr, 2.0);

    es::StretchParams p;
    p.stretch        = 1000.0f;
    p.windowSeconds  = 5.0f;
    p.pitchSemitones = -24.0f;
    p.blur           = 1.0f;
    p.rot            = 1.0f;
    p.scramble       = 1.0f;
    p.lowCutHz       = 15000.0f;
    p.highCutHz      = 100.0f;     // deliberately inverted
    p.width          = 1.0f;

    auto r = renderHops (source, p, sr, 2, 3);

    check (allFinite (r.audio), "every control at its limit stays finite");
    check (peakOf (r.audio) < 10.0, "and bounded", "peak " + juce::String (peakOf (r.audio), 4));

    es::StretchParams tiny;
    tiny.stretch       = 1.0f;
    tiny.windowSeconds = 0.001f;    // below the minimum order, must clamp

    auto small = renderHops (source, tiny, sr, 2, 8);

    check (allFinite (small.audio), "a sub-minimum window clamps instead of breaking");
    check (small.hopSize == (1 << (es::PaulStretch::minOrder - 1)),
           "window clamps to the minimum order",
           "hop " + juce::String (small.hopSize));

    es::StretchParams huge = tiny;
    huge.windowSeconds = 60.0f;     // above the maximum order

    auto big = renderHops (source, huge, sr, 2, 2);

    check (big.hopSize == (1 << (es::PaulStretch::maxOrder - 1)),
           "window clamps to the maximum order",
           "hop " + juce::String (big.hopSize));
    check (allFinite (big.audio), "the maximum window renders");
}

void testAlgorithms()
{
    section ("algorithms");

    constexpr double sr = 44100.0;

    // A band-limited source so the default 20 Hz / 20 kHz spectral cuts do not
    // muddy the reconstruction comparison.
    juce::AudioBuffer<float> chord (2, (int) (sr * 2.0));
    chord.clear();

    for (double f : { 220.0, 330.0, 440.0, 550.0 })
        for (int ch = 0; ch < 2; ++ch)
            for (int i = 0; i < chord.getNumSamples(); ++i)
                chord.setSample (ch, i, chord.getSample (ch, i)
                    + 0.2f * (float) std::sin (juce::MathConstants<double>::twoPi * f * i / sr));

    // --- vocoder: at ratio 1 a phase vocoder is a perfect reconstructor -----
    {
        es::StretchParams p;
        p.algorithm     = es::Algorithm::vocoder;
        p.stretch       = 1.0f;
        p.windowSeconds = 0.05f;

        auto r = renderHops (chord, p, sr, 2, 10);

        const auto c = correlate (r.audio.getReadPointer (0) + r.hopSize * 2,
                                  chord.getReadPointer (0)   + r.hopSize * 2,
                                  r.hopSize * 6);

        check (c > 0.99, "vocoder at 1x reconstructs the input",
               "correlation " + juce::String (c, 5));

        // The same signal through the random-phase path must NOT line up — that
        // is the whole difference between the two algorithms.
        es::StretchParams s = p;
        s.algorithm = es::Algorithm::smooth;

        auto sr2 = renderHops (chord, s, sr, 2, 10);
        const auto cs = correlate (sr2.audio.getReadPointer (0) + sr2.hopSize * 2,
                                   chord.getReadPointer (0)     + sr2.hopSize * 2,
                                   sr2.hopSize * 6);

        check (std::abs (cs) < 0.5, "smooth at 1x does not, because the phases are random",
               "correlation " + juce::String (cs, 5));
    }

    // --- vocoder holds pitch when actually stretching -----------------------
    {
        es::StretchParams p;
        p.algorithm     = es::Algorithm::vocoder;
        p.stretch       = 20.0f;
        p.windowSeconds = 0.25f;

        auto sine = makeSine (1000.0, sr, 2.0);
        auto r    = renderHops (sine, p, sr, 2, 12);
        auto f0   = dominantFrequency (r.audio, sr, 0, r.hopSize * 2);

        check (std::abs (f0 - 1000.0) < 15.0, "vocoder at 20x still reads 1 kHz",
               juce::String (f0, 2) + " Hz");

        const auto level = rmsOf (r.audio, 0, r.hopSize * 3, r.hopSize * 6);
        check (level > 0.05, "vocoder output is audible", "rms " + juce::String (level, 4));
        check (allFinite (r.audio), "vocoder output is finite");
    }

    // --- onset --------------------------------------------------------------
    {
        // Dense enough that every analysis window contains an attack, and the
        // onset list is taken from the generator so the two cannot drift apart.
        std::vector<int> positions;
        auto clicks = makeClickTrain (sr, 4.0, 20.0, &positions);

        std::vector<es::Onset> onsets;

        for (int position : positions)
            onsets.push_back ({ position, 1.0f });

        es::StretchParams p;
        p.algorithm     = es::Algorithm::onset;
        p.stretch       = 20.0f;
        p.windowSeconds = 0.05f;
        p.shape         = 0.0f;

        auto quiet = renderHops (clicks, p, sr, 2, 12, onsets);

        es::StretchParams s = p;
        s.algorithm = es::Algorithm::smooth;
        auto smooth = renderHops (clicks, s, sr, 2, 12, onsets);

        double worst = 0.0;

        for (int i = 0; i < quiet.audio.getNumSamples(); ++i)
            worst = juce::jmax (worst, std::abs ((double) quiet.audio.getSample (0, i)
                                                 - smooth.audio.getSample (0, i)));

        check (worst < 1.0e-9, "onset at sensitivity 0 is exactly smooth",
               "max difference " + juce::String (worst, 12));

        p.shape = 1.0f;
        auto fired = renderHops (clicks, p, sr, 2, 12, onsets);

        double changed = 0.0;

        for (int i = 0; i < fired.audio.getNumSamples(); ++i)
            changed = juce::jmax (changed, std::abs ((double) fired.audio.getSample (0, i)
                                                     - smooth.audio.getSample (0, i)));

        check (changed > 1.0e-4, "onset at sensitivity 1 re-articulates attacks",
               "max difference " + juce::String (changed, 5));
        check (allFinite (fired.audio), "onset output is finite");
        check (rmsOf (fired.audio, 0, fired.hopSize * 3, fired.hopSize * 6) > 0.01,
               "onset output is audible");
    }

    // --- grain --------------------------------------------------------------
    {
        es::StretchParams p;
        p.algorithm     = es::Algorithm::grain;
        p.stretch       = 20.0f;
        p.windowSeconds = 0.1f;
        p.shape         = 0.5f;

        auto noise = makeNoise (sr, 4.0, 0.25f);
        auto r     = renderHops (noise, p, sr, 2, 10);

        const auto level = rmsOf (r.audio, 0, r.hopSize * 2, r.hopSize * 6);
        const auto db    = 20.0 * std::log10 (juce::jmax (1.0e-9, level / 0.25));

        check (allFinite (r.audio), "grain output is finite");
        check (std::abs (db) < 4.0, "grain stays roughly at the source level",
               juce::String (db, 2) + " dB");
        check (peakOf (r.audio) < 4.0, "grain output is bounded",
               "peak " + juce::String (peakOf (r.audio), 3));

        // spray at 0 should be tamer than spray at 1
        p.shape = 0.0f;
        auto tight = renderHops (noise, p, sr, 2, 10);
        check (allFinite (tight.audio), "grain at zero spray is finite");
    }

    // --- hold ---------------------------------------------------------------
    {
        es::StretchParams p;
        p.algorithm     = es::Algorithm::hold;
        p.stretch       = 20.0f;
        p.windowSeconds = 0.1f;
        p.shape         = 1.0f;

        auto noise = makeNoise (sr, 4.0, 0.25f);
        auto r     = renderHops (noise, p, sr, 2, 12);

        const auto level = rmsOf (r.audio, 0, r.hopSize * 4, r.hopSize * 6);
        const auto db    = 20.0 * std::log10 (juce::jmax (1.0e-9, level / 0.25));

        check (allFinite (r.audio), "hold output is finite");
        check (std::abs (db) < 3.0, "hold stays near the source level",
               juce::String (db, 2) + " dB");

        // The point of hold is that the spectrum lags. Feed it a step — silence
        // then noise — and it should take far longer to arrive at full level.
        juce::AudioBuffer<float> step (2, (int) (sr * 4.0));
        step.clear();

        {
            auto tail = makeNoise (sr, 2.0, 0.25f);

            for (int ch = 0; ch < 2; ++ch)
                step.copyFrom (ch, (int) (sr * 2.0), tail, ch, 0, tail.getNumSamples());
        }

        es::StretchParams s;
        s.windowSeconds = 0.05f;
        s.stretch       = 1.0f;
        s.shape         = 1.0f;

        auto framesToFullLevel = [&] (es::Algorithm algorithm)
        {
            auto q = s;
            q.algorithm = algorithm;

            auto rendered = renderHops (step, q, sr, 2, 140);

            double settled = 0.0;

            for (int f = 118; f < 138; ++f)
                settled += rmsOf (rendered.audio, 0, f * rendered.hopSize, rendered.hopSize) / 20.0;

            for (int f = 0; f < 140; ++f)
                if (rmsOf (rendered.audio, 0, f * rendered.hopSize, rendered.hopSize) > settled * 0.5)
                    return f;

            return 140;
        };

        const int fastFrames = framesToFullLevel (es::Algorithm::smooth);
        const int slowFrames = framesToFullLevel (es::Algorithm::hold);

        check (slowFrames > fastFrames + 8, "hold lags a step change, smooth does not",
               juce::String (fastFrames) + " frames vs " + juce::String (slowFrames));
    }
}

void testAbsurdRatios()
{
    section ("absurd ratios");

    constexpr double sr = 44100.0;

    auto source = makeNoise (sr, 2.0);

    for (int a = 0; a < es::numAlgorithms; ++a)
    {
        es::StretchParams p;
        p.algorithm     = (es::Algorithm) a;
        p.stretch       = 1.0e15f;
        p.windowSeconds = 0.25f;

        auto r = renderHops (source, p, sr, 2, 4);

        check (allFinite (r.audio) && peakOf (r.audio) < 8.0,
               juce::String (es::toString ((es::Algorithm) a)) + " survives 1e15x",
               "peak " + juce::String (peakOf (r.audio), 4));
    }

    es::StretchParams p;
    p.stretch       = 1.0e15f;
    p.windowSeconds = 0.25f;

    auto r = renderHops (source, p, sr, 2, 4);

    check (r.readPosition < 1.0e-6, "at 1e15x the read head effectively stops",
           juce::String (r.readPosition, 12));

    const double total = 2.0 * 1.0e15;
    check (es::duration::format (total).contains ("million years"),
           "2 s at 1e15x reads as millions of years",
           es::duration::format (total));
}

void testDuration()
{
    section ("duration");

    using es::duration::format;
    using es::duration::parse;

    check (format (0.5)          == "0.50 s",  "sub-second",       format (0.5));
    check (format (95.0)         == "1:35",    "minutes",          format (95.0));
    check (format (3725.0)       == "1:02:05", "hours",            format (3725.0));
    check (format (86400.0 * 5)  .contains ("days"), "days",       format (86400.0 * 5));
    check (format (86400.0 * 900).contains ("years"), "years",     format (86400.0 * 900));
    check (format (3.156e16)     .contains ("billion years"), "billions of years",
           format (3.156e16));

    check (std::abs (parse ("90")            - 90.0)      < 1.0e-6, "bare number is seconds");
    check (std::abs (parse ("2:30")          - 150.0)     < 1.0e-6, "mm:ss");
    check (std::abs (parse ("1:02:05")       - 3725.0)    < 1.0e-6, "h:mm:ss");
    check (std::abs (parse ("3h")            - 10800.0)   < 1.0e-6, "3h");
    check (std::abs (parse ("45 min")        - 2700.0)    < 1.0e-6, "45 min");
    check (std::abs (parse ("2.5 days")      - 216000.0)  < 1.0e-6, "2.5 days");
    check (std::abs (parse ("1000 years")    - 3.15576e10) < 1.0e5, "1000 years");
    check (parse ("9.5 billion years") > 2.9e17
        && parse ("9.5 billion years") < 3.1e17,          "9.5 billion years",
           juce::String (parse ("9.5 billion years"), 0));
    check (parse ("nonsense") < 0.0, "rejects nonsense");
    check (parse ("") < 0.0,         "rejects empty");

    // round trip through the formatter
    for (double s : { 12.0, 300.0, 7200.0, 86400.0 * 3, 3.156e10 })
    {
        const auto back = parse (format (s));
        check (back > 0.0 && std::abs (back - s) < s * 0.02,
               "format then parse round-trips at " + format (s),
               juce::String (back, 1));
    }
}

//==============================================================================

juce::File writeTempSource (double sampleRate, double seconds)
{
    auto file = juce::File::getSpecialLocation (juce::File::tempDirectory)
                    .getChildFile ("extremestretch-selftest-source.wav");

    file.deleteFile();

    auto source = makeSine (440.0, sampleRate, seconds, 0.4f, 2);

    juce::WavAudioFormat wav;
    std::unique_ptr<juce::OutputStream> stream (file.createOutputStream());

    if (stream == nullptr)
        return {};

    auto writer = wav.createWriterFor (stream, juce::AudioFormatWriterOptions {}
                                                   .withSampleRate (sampleRate)
                                                   .withNumChannels (2)
                                                   .withBitsPerSample (24));

    if (writer == nullptr)
        return {};

    writer->writeFromAudioSampleBuffer (source, 0, source.getNumSamples());
    writer.reset();

    return file;
}

void testEngine()
{
    section ("engine");

    constexpr double sr = 44100.0;

    auto sourceFile = writeTempSource (sr, 1.0);
    check (sourceFile.existsAsFile(), "wrote a temporary source file");

    if (! sourceFile.existsAsFile())
        return;

    es::StretchEngine engine;
    engine.prepare (sr, 2);

    es::StretchParams params;
    params.stretch       = 12.0f;
    params.windowSeconds = 0.05f;
    engine.setParameterProvider ([params] { return params; });

    const auto error = engine.loadFile (sourceFile);
    check (error.isEmpty(), "loaded the file", error);
    check (engine.hasSource(), "engine reports a source");
    check (std::abs (engine.getSourceSeconds() - 1.0) < 0.01, "source length is right",
           juce::String (engine.getSourceSeconds(), 4) + " s");

    engine.start();

    // Pull audio the way an audio callback would and let the render thread keep up.
    constexpr int blockSize = 512;
    juce::AudioBuffer<float> block (2, blockSize);
    juce::AudioBuffer<float> collected (2, (int) (sr * 1.0));

    // Only count what readOutput actually delivered — a short read is an
    // underrun, and pasting its silence into the buffer would hide it.
    int written = 0;
    int spins   = 0;
    int empty   = 0;

    while (written < collected.getNumSamples() && spins < 20000)
    {
        const int got = engine.readOutput (block);
        ++spins;

        if (got <= 0)
        {
            ++empty;
            juce::Thread::sleep (1);
            continue;
        }

        const int n = juce::jmin (got, collected.getNumSamples() - written);

        for (int ch = 0; ch < 2; ++ch)
            collected.copyFrom (ch, written, block, ch, 0, n);

        written += n;
    }

    engine.stop();

    check (written >= collected.getNumSamples(), "the render thread kept the FIFO fed",
           juce::String (written) + " samples, " + juce::String (empty) + " empty reads of "
               + juce::String (spins));
    check (allFinite (collected), "engine output is finite");

    // The first blocks can legitimately be empty while the thread spins up.
    const auto settled = rmsOf (collected, 0, (int) (sr * 0.25), (int) (sr * 0.5));
    check (settled > 0.02, "engine output is audible", "rms " + juce::String (settled, 4));

    const auto f0 = dominantFrequency (collected, sr, 0, (int) (sr * 0.2));
    check (std::abs (f0 - 440.0) < 12.0, "engine output holds the source pitch",
           juce::String (f0, 2) + " Hz");

    // offline render
    auto rendered = juce::File::getSpecialLocation (juce::File::tempDirectory)
                        .getChildFile ("extremestretch-selftest-render.wav");

    const auto renderError = engine.renderToFile (rendered, 30.0, params, nullptr);
    check (renderError.isEmpty(), "rendered to a wav", renderError);

    if (renderError.isEmpty())
    {
        juce::AudioFormatManager formats;
        formats.registerBasicFormats();

        std::unique_ptr<juce::AudioFormatReader> reader (formats.createReaderFor (rendered));
        check (reader != nullptr, "the rendered wav reads back");

        if (reader != nullptr)
        {
            const double seconds  = (double) reader->lengthInSamples / reader->sampleRate;
            const double expected = 1.0 * 12.0;

            check (std::abs (seconds - expected) < 0.5, "rendered length is source x stretch",
                   juce::String (seconds, 3) + " s vs " + juce::String (expected, 3) + " s");

            juce::AudioBuffer<float> back ((int) reader->numChannels,
                                           (int) juce::jmin (reader->lengthInSamples, (juce::int64) (sr * 2)));
            reader->read (&back, 0, back.getNumSamples(), (juce::int64) (sr * 1), true, true);

            check (rmsOf (back) > 0.02, "the rendered file is not silence",
                   "rms " + juce::String (rmsOf (back), 4));
            check (allFinite (back), "the rendered file is finite");
        }
    }

    // --- seek ---------------------------------------------------------------
    engine.rewind();
    engine.start();
    engine.seekToSourceNormalised (0.75);

    bool seeked = false;

    for (int i = 0; i < 500 && ! seeked; ++i)
    {
        engine.readOutput (block);
        seeked = engine.getPlaybackProgress() > 0.7;
        juce::Thread::sleep (1);
    }

    check (seeked, "seeking jumps the playhead",
           "progress " + juce::String (engine.getPlaybackProgress(), 3));

    engine.stop();

    // --- onset detection ----------------------------------------------------
    {
        auto clickFile = juce::File::getSpecialLocation (juce::File::tempDirectory)
                             .getChildFile ("extremestretch-selftest-clicks.wav");

        clickFile.deleteFile();

        std::vector<int> positions;
        auto clicks = makeClickTrain (sr, 4.0, 4.0, &positions);   // 16 attacks
        std::unique_ptr<juce::OutputStream> stream (clickFile.createOutputStream());

        if (stream != nullptr)
        {
            juce::WavAudioFormat wav;
            auto writer = wav.createWriterFor (stream, juce::AudioFormatWriterOptions {}
                                                           .withSampleRate (sr)
                                                           .withNumChannels (2)
                                                           .withBitsPerSample (24));

            if (writer != nullptr)
            {
                writer->writeFromAudioSampleBuffer (clicks, 0, clicks.getNumSamples());
                writer.reset();

                es::StretchEngine detector;
                detector.prepare (sr, 2);
                const auto clickError = detector.loadFile (clickFile);

                check (clickError.isEmpty(), "loaded the click train", clickError);

                const int found = detector.getNumOnsets();
                check (found >= 12 && found <= 20, "onset detector finds the 16 attacks",
                       juce::String (found) + " found");

                detector.release();
            }
        }

        clickFile.deleteFile();
    }

    engine.release();
    rendered.deleteFile();
    sourceFile.deleteFile();
}

} // namespace

//==============================================================================

int main()
{
    juce::ScopedJuceInitialiser_GUI juceInit;

    std::puts ("Extreme Stretch — self-test");

    testFftAssumptions();
    testWindowMath();
    testUnityGain();
    testPitchIsPreserved();
    testTimeBase();
    testDestruction();
    testSpectralFilter();
    testStereo();
    testMonoSource();
    testDeclick();
    testExtremeSettings();
    testAlgorithms();
    testAbsurdRatios();
    testDuration();
    testEngine();

    std::printf ("\n%s  (%d/%d)\n",
                 checksFailed == 0 ? "ALL PASS" : "FAILURES",
                 checksRun - checksFailed, checksRun);

    return checksFailed == 0 ? 0 : 1;
}
