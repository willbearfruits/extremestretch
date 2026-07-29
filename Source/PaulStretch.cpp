#include "PaulStretch.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace es
{

const char* toString (Algorithm a) noexcept
{
    switch (a)
    {
        case Algorithm::smooth:  return "Smooth";
        case Algorithm::onset:   return "Onset";
        case Algorithm::vocoder: return "Vocoder";
        case Algorithm::grain:   return "Grain";
        case Algorithm::hold:    return "Hold";
    }

    return "Smooth";
}

const char* shapeCaptionFor (Algorithm a) noexcept
{
    switch (a)
    {
        case Algorithm::smooth:  return "SHAPE";
        case Algorithm::onset:   return "SENSITIVITY";
        case Algorithm::vocoder: return "COHERENCE";
        case Algorithm::grain:   return "SPRAY";
        case Algorithm::hold:    return "HOLD";
    }

    return "SHAPE";
}

bool isSpectral (Algorithm a) noexcept
{
    return a != Algorithm::grain;
}

namespace
{
    constexpr float twoPi = juce::MathConstants<float>::twoPi;
    constexpr float pi    = juce::MathConstants<float>::pi;

    /** Wraps a phase into -pi..pi. */
    inline float princarg (float phase) noexcept
    {
        return phase - twoPi * std::floor (phase / twoPi + 0.5f);
    }

    /** Linear interpolation into a buffer that wraps around. */
    inline float readWrapped (const float* src, int length, double position) noexcept
    {
        double wrapped = std::fmod (position, (double) length);

        if (wrapped < 0.0)
            wrapped += length;

        const auto i0 = (int) wrapped;
        const int  i1 = (i0 + 1 < length) ? i0 + 1 : 0;
        const auto f  = (float) (wrapped - (double) i0);

        return src[i0] * (1.0f - f) + src[i1] * f;
    }
}

//==============================================================================

int PaulStretch::orderForSeconds (float seconds, double sampleRate) noexcept
{
    const double sr      = juce::jmax (1.0, sampleRate);
    const double samples = juce::jlimit (0.002, 12.0, (double) seconds) * sr;
    const int    order   = (int) std::lround (std::log2 (juce::jmax (16.0, samples)));
    return juce::jlimit (minOrder, maxOrder, order);
}

void PaulStretch::prepare (double sr, int channels)
{
    sampleRate  = juce::jmax (1.0, sr);
    numChannels = juce::jmax (1, channels);

    fftOrder = 0;
    fft.reset();
    display.assign ((size_t) displayBins, 0.0f);

    setOrder (orderForSeconds (0.25f, sampleRate));
    reset();
}

void PaulStretch::reset()
{
    for (auto& t : tail)
        std::fill (t.begin(), t.end(), 0.0f);

    std::fill (health.begin(), health.end(), 1.0f);
    std::fill (display.begin(), display.end(), 0.0f);

    rotActive         = false;
    stateIsStale      = true;
    readPos           = 0.0;
    lastFrameStart    = 0;
    lastOnsetConsumed = -1;
}

void PaulStretch::setReadPosition (double p) noexcept
{
    readPos           = p;
    lastFrameStart    = (long long) std::floor (p);
    lastOnsetConsumed = -1;
    stateIsStale      = true;
}

void PaulStretch::setOrder (int order)
{
    order = juce::jlimit (minOrder, maxOrder, order);

    if (order == fftOrder && fft != nullptr)
        return;

    fftOrder   = order;
    windowSize = 1 << order;
    fft        = std::make_unique<juce::dsp::FFT> (order);

    const auto bins = (size_t) getNumBins();

    windowPaul    .resize ((size_t) windowSize);
    windowSqrtHann.resize ((size_t) windowSize);
    windowHann    .resize ((size_t) windowSize);

    for (int i = 0; i < windowSize; ++i)
    {
        const double x = 2.0 * i / (double) (windowSize - 1) - 1.0;
        const double h = 0.5 - 0.5 * std::cos (juce::MathConstants<double>::twoPi
                                                   * i / (double) (windowSize - 1));

        windowPaul    [(size_t) i] = (float) std::sqrt (juce::jmax (0.0, 1.0 - std::pow (std::abs (x), 2.5)));
        windowHann    [(size_t) i] = (float) h;
        windowSqrtHann[(size_t) i] = (float) std::sqrt (h);
    }

    fftData .assign ((size_t) numChannels, std::vector<float> ((size_t) windowSize * 2, 0.0f));
    mags    .assign ((size_t) numChannels, std::vector<float> (bins, 0.0f));
    origMags.assign ((size_t) numChannels, std::vector<float> (bins, 0.0f));
    magTmp  .assign ((size_t) numChannels, std::vector<float> (bins, 0.0f));
    tail    .assign ((size_t) numChannels, std::vector<float> ((size_t) getHopSize(), 0.0f));

    prevPhase.assign ((size_t) numChannels, std::vector<float> (bins, 0.0f));
    sumPhase .assign ((size_t) numChannels, std::vector<float> (bins, 0.0f));
    binFreq  .assign ((size_t) numChannels, std::vector<float> (bins, 0.0f));
    heldMags .assign ((size_t) numChannels, std::vector<float> (bins, 0.0f));

    health.assign (bins, 1.0f);
    prefix.assign (bins + 1, 0.0f);

    swapA.clear();
    swapB.clear();
    swapA.reserve (bins);
    swapB.reserve (bins);

    if (display.size() != (size_t) displayBins)
        display.assign ((size_t) displayBins, 0.0f);

    rotActive    = false;
    stateIsStale = true;
}

//==============================================================================

void PaulStretch::gatherFrame (const juce::AudioBuffer<float>& source, int sourceChannel,
                               int destChannel, const float* w)
{
    const int    srcLen = source.getNumSamples();
    const float* src    = source.getReadPointer (sourceChannel);
    float*       d      = fftData[(size_t) destChannel].data();

    auto start = (long long) std::floor (readPos);
    start %= (long long) srcLen;

    if (start < 0)
        start += srcLen;

    int pos = (int) start;
    int i   = 0;

    while (i < windowSize)
    {
        const int n = juce::jmin (windowSize - i, srcLen - pos);
        juce::FloatVectorOperations::multiply (d + i, src + pos, w + i, n);

        i   += n;
        pos += n;

        if (pos >= srcLen)
            pos = 0;
    }

    // The real-only transform wants 2 * fftSize floats of scratch.
    juce::FloatVectorOperations::clear (d + windowSize, windowSize);
}

void PaulStretch::updateRot (const StretchParams& p)
{
    const int bins = getNumBins();

    rotActive = false;

    // Bins do not fade — they are *killed*, and the damage persists across
    // frames. Hold the knob up and the drone erodes down to a skeleton of
    // whichever partials survive.
    if (p.rot > 0.0005f)
    {
        rotActive = true;

        const int kills = (int) (p.rot * p.rot * (float) bins * 0.03f);

        for (int i = 0; i < kills; ++i)
            health[(size_t) rng.nextInt (bins)] *= 0.3f;
    }

    // Backing the knob off heals, so there is a way out of the damage.
    const float heal = (1.0f - p.rot) * 0.05f;

    if (heal > 0.0f)
    {
        for (int k = 0; k < bins; ++k)
        {
            auto& h = health[(size_t) k];

            if (h < 0.9999f)
            {
                h += heal * (1.0f - h);
                rotActive = true;
            }
        }
    }
}

void PaulStretch::buildScramble (const StretchParams& p)
{
    swapA.clear();
    swapB.clear();

    if (p.scramble <= 0.001f)
        return;

    const int bins = getNumBins();
    const int n    = (int) (p.scramble * (float) bins * 0.25f);
    const int span = juce::jmax (1, (int) (p.scramble * p.scramble * (float) bins * 0.2f));

    // Built once per frame and applied identically to every channel, so the
    // stereo image survives the mangling.
    for (int i = 0; i < n; ++i)
    {
        const int a = rng.nextInt (bins);
        const int b = juce::jlimit (0, bins - 1, a + rng.nextInt (2 * span + 1) - span);

        swapA.push_back (a);
        swapB.push_back (b);
    }
}

void PaulStretch::shapeMagnitudes (int channel, const StretchParams& p)
{
    const int bins = getNumBins();
    float* const m = mags  [(size_t) channel].data();
    float* const t = magTmp[(size_t) channel].data();

    // --- hold ---------------------------------------------------------------
    // Time-domain smearing: average the magnitude spectrum over recent frames.
    // Pairs with blur, which smears in the other axis.
    if (p.algorithm == Algorithm::hold && p.shape > 0.001f)
    {
        auto* held = heldMags[(size_t) channel].data();
        const float a = 1.0f / (1.0f + p.shape * 30.0f);

        if (stateIsStale)
            std::memcpy (held, m, sizeof (float) * (size_t) bins);

        for (int k = 0; k < bins; ++k)
        {
            held[k] += (m[k] - held[k]) * a;
            m[k]     = held[k];
        }
    }

    // --- pitch --------------------------------------------------------------
    // Resampling the magnitude spectrum is a *clean* pitch shift here: there
    // are no phases left to smear, so none of the usual vocoder artefacts.
    if (std::abs (p.pitchSemitones) > 0.001f)
    {
        const double ratio = std::pow (2.0, (double) p.pitchSemitones / 12.0);

        for (int k = 0; k < bins; ++k)
        {
            const double s  = (double) k / ratio;
            const int    i0 = (int) s;

            if (i0 < 0 || i0 >= bins - 1)
            {
                t[k] = 0.0f;
                continue;
            }

            const auto f = (float) (s - (double) i0);
            t[k] = m[i0] * (1.0f - f) + m[i0 + 1] * f;
        }

        std::memcpy (m, t, sizeof (float) * (size_t) bins);
    }

    // --- blur ---------------------------------------------------------------
    if (p.blur > 0.001f)
    {
        prefix[0] = 0.0f;

        for (int k = 0; k < bins; ++k)
            prefix[(size_t) k + 1] = prefix[(size_t) k] + m[k];

        const float amount = p.blur * p.blur;

        double before = 0.0, after = 0.0;

        for (int k = 0; k < bins; ++k)
        {
            // The radius grows with the bin index, so the smear is constant in
            // octaves rather than in hertz. A fixed bin radius sounds wrong:
            // it barely touches the top of the spectrum while obliterating the
            // bass, where the partials that carry pitch actually live.
            const int r  = juce::jmax (1, (int) (amount * (float) k * 0.5f));
            const int lo = juce::jmax (0, k - r);
            const int hi = juce::jmin (bins, k + r + 1);

            t[k] = (prefix[(size_t) hi] - prefix[(size_t) lo]) / (float) (hi - lo);

            before += (double) m[k] * m[k];
            after  += (double) t[k] * t[k];
        }

        // Smearing flattens peaks and would otherwise drop the level, so put
        // the energy back.
        const auto comp = (float) (after > 1.0e-20 ? std::sqrt (before / after) : 1.0);

        for (int k = 0; k < bins; ++k)
            m[k] = t[k] * comp;
    }

    // --- scramble -----------------------------------------------------------
    for (size_t s = 0; s < swapA.size(); ++s)
        std::swap (m[swapA[s]], m[swapB[s]]);

    // --- rot ----------------------------------------------------------------
    if (rotActive)
        for (int k = 0; k < bins; ++k)
            m[k] *= health[(size_t) k];

    // --- spectral filter ----------------------------------------------------
    const auto binHz = (float) (sampleRate / (double) windowSize);
    const float loBin = p.lowCutHz  / binHz;
    const float hiBin = p.highCutHz / binHz;

    const bool doHp = loBin > 0.5f;
    const bool doLp = hiBin < (float) bins;

    if (doHp || doLp)
    {
        for (int k = 1; k < bins; ++k)
        {
            float g = 1.0f;

            if (doHp)
            {
                const float a  = (float) k / loBin;
                const float a4 = a * a * a * a;
                g *= a4 / (1.0f + a4);
            }

            if (doLp)
            {
                const float b  = (float) k / hiBin;
                const float b4 = b * b * b * b;
                g *= 1.0f / (1.0f + b4);
            }

            m[k] *= g;
        }
    }

    m[0] = 0.0f; // no DC offset in the output, ever
}

void PaulStretch::updateDisplaySpectrum()
{
    const int bins  = getNumBins();
    const auto norm = 1.0f / (float) juce::jmax (1, windowSize / 4);

    for (int i = 0; i < displayBins; ++i)
    {
        const double f0 = std::pow ((double) bins, (double) i       / displayBins);
        const double f1 = std::pow ((double) bins, (double) (i + 1) / displayBins);

        const int a = juce::jlimit (1, bins - 1, (int) f0);
        const int b = juce::jlimit (a + 1, bins, (int) f1);

        float peak = 0.0f;

        for (int k = a; k < b; ++k)
            peak = juce::jmax (peak, mags[0][(size_t) k]);

        display[(size_t) i] = peak * norm;
    }
}

bool PaulStretch::consumeOnset (const std::vector<Onset>& onsets, const StretchParams& p)
{
    if (onsets.empty() || p.shape <= 0.001f)
        return false;

    const float     threshold = 1.0f - p.shape;
    const long long start     = (long long) std::floor (readPos);
    const long long end       = start + windowSize;

    auto it = std::lower_bound (onsets.begin(), onsets.end(), start,
                                [] (const Onset& o, long long v) { return (long long) o.position < v; });

    for (; it != onsets.end() && (long long) it->position < end; ++it)
        if (it->position > lastOnsetConsumed && it->strength >= threshold)
        {
            lastOnsetConsumed = it->position;
            return true;
        }

    return false;
}

void PaulStretch::overlapAdd (float* const* out, int numOutChannels)
{
    const int hop = getHopSize();

    for (int ch = 0; ch < numChannels; ++ch)
    {
        const float* d  = fftData[(size_t) ch].data();
        auto&        tl = tail[(size_t) ch];

        if (ch < numOutChannels)
            juce::FloatVectorOperations::add (out[ch], d, tl.data(), hop);

        std::memcpy (tl.data(), d + hop, sizeof (float) * (size_t) hop);
    }

    // Mono source into a stereo bus: mirror rather than leaving silence.
    for (int ch = numChannels; ch < numOutChannels; ++ch)
        juce::FloatVectorOperations::copy (out[ch], out[juce::jmax (0, numChannels - 1)], hop);
}

//==============================================================================

void PaulStretch::renderGrainHop (const juce::AudioBuffer<float>& source, float* const* out,
                                  int numOutChannels, const StretchParams& p)
{
    constexpr int grainsPerFrame = 3;

    const int    srcLen = source.getNumSamples();
    const int    srcCh  = source.getNumChannels();
    const double ratio  = std::pow (2.0, (double) p.pitchSemitones / 12.0);
    const auto   norm   = 1.0f / std::sqrt ((float) grainsPerFrame);

    for (int ch = 0; ch < numChannels; ++ch)
        juce::FloatVectorOperations::clear (fftData[(size_t) ch].data(), windowSize);

    for (int g = 0; g < grainsPerFrame; ++g)
    {
        // Spray: each grain reads from a jittered offset, so the cloud stops
        // sounding like one looped window.
        const double jitter = (double) (rng.nextFloat() * 2.0f - 1.0f) * p.shape * (double) windowSize * 2.0;
        const double start  = readPos + jitter;
        const float  pan    = 0.5f + (rng.nextFloat() - 0.5f) * p.width;

        for (int ch = 0; ch < numChannels; ++ch)
        {
            const float* src = source.getReadPointer (juce::jmin (ch, srcCh - 1));
            float*       d   = fftData[(size_t) ch].data();

            const float gain = (numChannels < 2) ? norm
                                                 : norm * std::sqrt (ch == 0 ? 1.0f - pan : pan)
                                                        * juce::MathConstants<float>::sqrt2;

            for (int i = 0; i < windowSize; ++i)
                d[i] += readWrapped (src, srcLen, start + (double) i * ratio)
                            * windowHann[(size_t) i] * gain;
        }
    }

    // Windowed once only, and Hann sums to exactly 1.0 at 50% overlap.
    overlapAdd (out, numOutChannels);
}

//==============================================================================

void PaulStretch::renderHop (const juce::AudioBuffer<float>& source,
                             const std::vector<Onset>& onsets,
                             float* const* out,
                             int numOutChannels,
                             const StretchParams& p)
{
    const int hop  = getHopSize();
    const int bins = getNumBins();

    for (int ch = 0; ch < numOutChannels; ++ch)
        juce::FloatVectorOperations::clear (out[ch], hop);

    if (fft == nullptr || source.getNumSamples() <= 0 || source.getNumChannels() <= 0)
        return;

    const auto advance = [this, &p, hop]
    {
        if (! p.freeze)
            readPos += (double) hop / (double) juce::jmax (1.0f, p.stretch);
    };

    if (p.algorithm == Algorithm::grain)
    {
        renderGrainHop (source, out, numOutChannels, p);
        stateIsStale = true;   // spectral state is meaningless after grain frames
        advance();
        return;
    }

    const int srcCh = source.getNumChannels();

    const bool coherentFrame = (p.algorithm == Algorithm::onset) && consumeOnset (onsets, p);
    const bool isVocoder     = (p.algorithm == Algorithm::vocoder);

    // Coherent addition needs sum(w_a * w_s) flat; random phase needs sum(w^2)
    // flat. Different windows.
    const float* const w = isVocoder ? windowSqrtHann.data() : windowPaul.data();

    const auto frameStart   = (long long) std::floor (readPos);
    const auto analysisHop  = (double) (frameStart - lastFrameStart);
    lastFrameStart = frameStart;

    // 1. window + forward transform + magnitudes
    for (int ch = 0; ch < numChannels; ++ch)
    {
        gatherFrame (source, juce::jmin (ch, srcCh - 1), ch, w);

        fft->performRealOnlyForwardTransform (fftData[(size_t) ch].data(), true);

        const float* d = fftData[(size_t) ch].data();
        float*       m = mags[(size_t) ch].data();
        float*       o = origMags[(size_t) ch].data();

        for (int k = 0; k < bins; ++k)
        {
            m[k] = std::sqrt (d[2 * k] * d[2 * k] + d[2 * k + 1] * d[2 * k + 1]);
            o[k] = m[k];
        }
    }

    // 2. vocoder phase tracking, before the magnitudes get mangled
    if (isVocoder)
    {
        for (int ch = 0; ch < numChannels; ++ch)
        {
            const float* d  = fftData[(size_t) ch].data();
            float*       pp = prevPhase[(size_t) ch].data();
            float*       bf = binFreq[(size_t) ch].data();
            float*       sp = sumPhase[(size_t) ch].data();

            for (int k = 0; k < bins; ++k)
            {
                const float phase = std::atan2 (d[2 * k + 1], d[2 * k]);

                if (analysisHop > 0.0)
                {
                    // Unwrap against the phase advance a bin-centre sinusoid
                    // would have made over the analysis hop; what is left is
                    // the bin's true deviation from its centre frequency.
                    const auto expected = (float) (twoPi * k * analysisHop / windowSize);
                    const float deviation = princarg (phase - pp[k] - expected);

                    bf[k] = (float) ((expected + deviation) / analysisHop);
                }
                else if (stateIsStale)
                {
                    // Stretch is so extreme the read head has not moved a whole
                    // sample. Fall back to the bin centre frequency.
                    bf[k] = (float) (twoPi * k / windowSize);
                }

                if (stateIsStale)
                    sp[k] = phase;
                else
                    sp[k] += bf[k] * (float) hop;

                pp[k] = phase;
            }
        }
    }

    // 3. magnitude domain — rot and scramble are shared so every channel takes
    //    identical damage
    updateRot (p);
    buildScramble (p);

    for (int ch = 0; ch < numChannels; ++ch)
        shapeMagnitudes (ch, p);

    updateDisplaySpectrum();

    // 4. resynthesis phase
    if (isVocoder)
    {
        for (int ch = 0; ch < numChannels; ++ch)
        {
            float*       d  = fftData[(size_t) ch].data();
            const float* m  = mags[(size_t) ch].data();
            const float* sp = sumPhase[(size_t) ch].data();

            for (int k = 0; k < bins; ++k)
            {
                if (k == 0 || k == bins - 1)
                {
                    d[2 * k] = d[2 * k + 1] = 0.0f;
                    continue;
                }

                d[2 * k]     = m[k] * std::cos (sp[k]);
                d[2 * k + 1] = m[k] * std::sin (sp[k]);
            }
        }
    }
    else if (coherentFrame)
    {
        // An attack: keep the source's own phases so the transient survives,
        // and just apply the magnitude shaping as a per-bin gain.
        for (int ch = 0; ch < numChannels; ++ch)
        {
            float*       d = fftData[(size_t) ch].data();
            const float* m = mags[(size_t) ch].data();
            const float* o = origMags[(size_t) ch].data();

            for (int k = 0; k < bins; ++k)
            {
                if (k == 0 || k == bins - 1)
                {
                    d[2 * k] = d[2 * k + 1] = 0.0f;
                    continue;
                }

                const float g = m[k] / juce::jmax (1.0e-12f, o[k]);
                d[2 * k]     *= g;
                d[2 * k + 1] *= g;
            }
        }
    }
    else
    {
        for (int k = 0; k < bins; ++k)
        {
            const float base = rng.nextFloat() * twoPi;

            for (int ch = 0; ch < numChannels; ++ch)
            {
                float* d = fftData[(size_t) ch].data();

                // DC and Nyquist have to stay real for the inverse transform.
                if (k == 0 || k == bins - 1)
                {
                    d[2 * k] = d[2 * k + 1] = 0.0f;
                    continue;
                }

                float phase = base;

                // Channels share a phase at width 0 and drift apart from there;
                // full decorrelation is what gives paulstretch its huge stereo.
                if (ch > 0 && p.width > 0.0f)
                    phase += p.width * (rng.nextFloat() * 2.0f - 1.0f) * pi;

                const float m = mags[(size_t) ch][(size_t) k];

                d[2 * k]     = m * std::cos (phase);
                d[2 * k + 1] = m * std::sin (phase);
            }
        }
    }

    // 5. inverse, synthesis window, overlap-add at 50%
    for (int ch = 0; ch < numChannels; ++ch)
    {
        float* d = fftData[(size_t) ch].data();

        fft->performRealOnlyInverseTransform (d);
        juce::FloatVectorOperations::multiply (d, w, windowSize);
    }

    overlapAdd (out, numOutChannels);

    stateIsStale = false;
    advance();
}

void PaulStretch::flushTail (float* const* out, int numOutChannels)
{
    const int hop = getHopSize();

    for (int ch = 0; ch < numOutChannels; ++ch)
    {
        const int src = juce::jmin (ch, numChannels - 1);
        juce::FloatVectorOperations::copy (out[ch], tail[(size_t) src].data(), hop);
    }

    for (auto& t : tail)
        std::fill (t.begin(), t.end(), 0.0f);
}

} // namespace es
