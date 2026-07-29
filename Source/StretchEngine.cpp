#include "StretchEngine.h"

#include <cmath>

namespace es
{

StretchEngine::StretchEngine()
    : juce::Thread ("extreme-stretch-render")
{
    formats.registerBasicFormats();
    spectrumSnapshot.assign ((size_t) PaulStretch::displayBins, 0.0f);
}

StretchEngine::~StretchEngine()
{
    release();
}

void StretchEngine::prepare (double newSampleRate, int newNumChannels)
{
    const bool rateChanged = std::abs (newSampleRate - sampleRate) > 1.0e-6;

    stopThread (2000);

    sampleRate  = juce::jmax (1.0, newSampleRate);
    numChannels = juce::jlimit (1, 8, newNumChannels);

    fifoBuffer.setSize (numChannels, fifoCapacity, false, true, true);
    staging   .setSize (numChannels, maxHop,       false, true, true);

    captureBuffer.setSize (numChannels,
                           juce::jmax (maxHop * 2, (int) (captureSeconds * sampleRate)),
                           false, true, true);
    captureBuffer.clear();
    captureWrite.store (0);

    stretcher.prepare (sampleRate, numChannels);

    {
        const juce::ScopedLock sl (sourceLock);

        if (rateChanged || fileBuffer.getNumSamples() == 0)
            rebuildFileBuffer();
    }

    {
        const juce::SpinLock::ScopedLockType sl (fifoLock);
        fifo.reset();
    }

    resetPending.store (true);
    producerDone.store (false);
    underruns.store (0);
    samplesConsumed.store (0);

    startThread (juce::Thread::Priority::normal);
}

void StretchEngine::release()
{
    stopThread (2000);
    playing.store (false);
}

//==============================================================================

juce::String StretchEngine::loadFile (const juce::File& file)
{
    if (! file.existsAsFile())
        return "File does not exist: " + file.getFullPathName();

    std::unique_ptr<juce::AudioFormatReader> reader (formats.createReaderFor (file));

    if (reader == nullptr)
        return "Unsupported or unreadable audio file: " + file.getFileName();

    const auto length = (int) juce::jmin (reader->lengthInSamples, (juce::int64) 1'000'000'000);

    if (length <= 0)
        return "That file contains no audio.";

    juce::AudioBuffer<float> loaded ((int) juce::jmax (1u, reader->numChannels), length);

    if (! reader->read (&loaded, 0, length, 0, true, true))
        return "Failed while reading " + file.getFileName();

    {
        const juce::ScopedLock sl (sourceLock);

        rawBuffer.makeCopyOf (loaded);
        rawSampleRate = reader->sampleRate > 0.0 ? reader->sampleRate : sampleRate;
        sourceName    = file.getFileName();

        rebuildFileBuffer();
        computeOnsets();
    }

    setSourceMode (SourceMode::file);
    rewind();

    return {};
}

void StretchEngine::clearSource()
{
    stop();

    const juce::ScopedLock sl (sourceLock);

    rawBuffer.setSize (0, 0);
    fileBuffer.setSize (numChannels, 0);
    onsets.clear();
    rawSampleRate = 0.0;
    sourceName.clear();
    sourceSamples.store (0);
    numOnsets.store (0);
}

void StretchEngine::computeOnsets()
{
    // Caller holds sourceLock. Spectral flux over the resampled source, peak
    // picked against a local average. Algorithm::onset uses these to decide
    // which frames get the source's own phases back so attacks survive.
    onsets.clear();
    numOnsets.store (0);

    const int length = fileBuffer.getNumSamples();

    constexpr int order = 10;
    constexpr int size  = 1 << order;
    constexpr int hop   = size / 2;
    constexpr int bins  = size / 2 + 1;

    if (length < size * 2 || fileBuffer.getNumChannels() <= 0)
        return;

    juce::dsp::FFT fft (order);

    std::vector<float> data ((size_t) size * 2, 0.0f);
    std::vector<float> window ((size_t) size);
    std::vector<float> previous ((size_t) bins, 0.0f);
    std::vector<float> flux;

    for (int i = 0; i < size; ++i)
        window[(size_t) i] = 0.5f - 0.5f * std::cos (juce::MathConstants<float>::twoPi
                                                         * (float) i / (float) (size - 1));

    const int channels = fileBuffer.getNumChannels();
    const int frames   = (length - size) / hop;

    flux.reserve ((size_t) juce::jmax (0, frames));

    for (int f = 0; f < frames; ++f)
    {
        const int start = f * hop;

        for (int i = 0; i < size; ++i)
        {
            float sum = 0.0f;

            for (int ch = 0; ch < channels; ++ch)
                sum += fileBuffer.getSample (ch, start + i);

            data[(size_t) i] = (sum / (float) channels) * window[(size_t) i];
        }

        std::fill (data.begin() + size, data.end(), 0.0f);
        fft.performRealOnlyForwardTransform (data.data(), true);

        float sum = 0.0f;

        for (int k = 1; k < bins - 1; ++k)
        {
            const float m = std::sqrt (data[(size_t) (2 * k)]     * data[(size_t) (2 * k)]
                                     + data[(size_t) (2 * k + 1)] * data[(size_t) (2 * k + 1)]);

            const float rise = m - previous[(size_t) k];

            if (rise > 0.0f)
                sum += rise;

            previous[(size_t) k] = m;
        }

        flux.push_back (sum);
    }

    if (flux.size() < 8)
        return;

    // Peak pick: a local maximum that also stands clear of its neighbourhood.
    constexpr int local   = 3;
    constexpr int context = 12;

    float strongest = 0.0f;

    for (int i = local; i < (int) flux.size() - local; ++i)
    {
        const float v = flux[(size_t) i];

        bool isPeak = true;

        for (int j = -local; j <= local && isPeak; ++j)
            if (j != 0 && flux[(size_t) (i + j)] >= v)
                isPeak = false;

        if (! isPeak)
            continue;

        double mean  = 0.0;
        int    count = 0;

        for (int j = juce::jmax (0, i - context); j < juce::jmin ((int) flux.size(), i + context); ++j)
        {
            mean += flux[(size_t) j];
            ++count;
        }

        mean /= juce::jmax (1, count);

        if ((double) v < mean * 1.3)
            continue;

        onsets.push_back ({ i * hop, v });
        strongest = juce::jmax (strongest, v);
    }

    for (auto& o : onsets)
        o.strength = strongest > 0.0f ? o.strength / strongest : 0.0f;

    numOnsets.store ((int) onsets.size());
}

void StretchEngine::rebuildFileBuffer()
{
    // Caller holds sourceLock.
    if (rawBuffer.getNumSamples() <= 0 || rawSampleRate <= 0.0)
    {
        fileBuffer.setSize (numChannels, 0);
        sourceSamples.store (0);
        return;
    }

    const double ratio  = rawSampleRate / sampleRate;
    const int    rawLen = rawBuffer.getNumSamples();

    // Match the engine rate on load rather than at read time — the FFT bin
    // frequencies have to mean what they say for the spectral filter and the
    // pitch shift to be honest.
    const bool sameRate = std::abs (ratio - 1.0) < 1.0e-9;
    const int  outLen   = sameRate ? rawLen
                                   : juce::jmax (1, (int) std::floor ((double) rawLen / ratio) - 8);

    fileBuffer.setSize (numChannels, outLen, false, true, true);
    fileBuffer.clear();

    for (int ch = 0; ch < numChannels; ++ch)
    {
        const int srcCh = juce::jmin (ch, rawBuffer.getNumChannels() - 1);

        if (sameRate)
        {
            fileBuffer.copyFrom (ch, 0, rawBuffer, srcCh, 0, juce::jmin (outLen, rawLen));
        }
        else
        {
            juce::LagrangeInterpolator interp;
            interp.reset();
            interp.process (ratio,
                            rawBuffer.getReadPointer (srcCh),
                            fileBuffer.getWritePointer (ch),
                            outLen);
        }
    }

    sourceSamples.store (fileBuffer.getNumSamples());
}

juce::String StretchEngine::getSourceName() const
{
    const juce::ScopedLock sl (sourceLock);
    return sourceName;
}

double StretchEngine::getSourceSeconds() const noexcept
{
    return (double) sourceSamples.load() / sampleRate;
}

void StretchEngine::setSourceMode (SourceMode mode)
{
    if (sourceMode.exchange (mode) != mode)
        resetPending.store (true);
}

//==============================================================================

void StretchEngine::start()
{
    if (sourceMode.load() == SourceMode::file && ! hasSource())
        return;

    resetPending.store (true);
    producerDone.store (false);
    underruns.store (0);
    samplesConsumed.store (0);

    {
        const juce::SpinLock::ScopedLockType sl (fifoLock);
        fifo.reset();
    }

    playing.store (true);
    notify();
}

void StretchEngine::stop()
{
    playing.store (false);
}

void StretchEngine::rewind()
{
    const bool wasPlaying = playing.load();

    playing.store (false);
    resetPending.store (true);
    producerDone.store (false);
    samplesConsumed.store (0);

    {
        const juce::SpinLock::ScopedLockType sl (fifoLock);
        fifo.reset();
    }

    if (wasPlaying)
        playing.store (true);

    notify();
}

void StretchEngine::seekToSourceNormalised (double position)
{
    pendingSeek.store (juce::jlimit (0.0, 1.0, position));
    notify();
}

double StretchEngine::sourcePositionInSamples() const noexcept
{
    // The render thread runs ahead of what is audible by whatever is sitting in
    // the FIFO. That is `ready` output samples, which is ready/stretch samples
    // of *source* — subtract it so the playhead matches what is being heard.
    const double stretch = juce::jmax (1.0, lastStretch.load());
    const double ahead   = (double) fifo.getNumReady() / stretch;

    return juce::jmax (0.0, readPosSnapshot.load() - ahead);
}

double StretchEngine::getTotalOutputSeconds() const noexcept
{
    if (sourceMode.load() == SourceMode::live)
        return 0.0;

    return getSourceSeconds() * juce::jmax (1.0, lastStretch.load());
}

double StretchEngine::getOutputSeconds() const noexcept
{
    if (sourceMode.load() == SourceMode::live)
        return (double) samplesConsumed.load() / sampleRate;

    return sourcePositionInSamples() * juce::jmax (1.0, lastStretch.load()) / sampleRate;
}

double StretchEngine::getPlaybackProgress() const noexcept
{
    const double length = (double) sourceSamples.load();
    return length > 0.0 ? juce::jlimit (0.0, 1.0, sourcePositionInSamples() / length) : 0.0;
}

void StretchEngine::setParameterProvider (std::function<StretchParams()> provider)
{
    const juce::ScopedLock sl (providerLock);
    paramProvider = std::move (provider);
}

StretchParams StretchEngine::fetchParams() const
{
    std::function<StretchParams()> fn;

    {
        const juce::ScopedLock sl (providerLock);
        fn = paramProvider;
    }

    return fn ? fn() : StretchParams {};
}

//==============================================================================

void StretchEngine::run()
{
    while (! threadShouldExit())
    {
        if (! produce())
            wait (4);
    }
}

bool StretchEngine::produce()
{
    if (resetPending.exchange (false))
    {
        const juce::SpinLock::ScopedLockType sl (fifoLock);

        fifo.reset();
        stretcher.reset();
        producerDone.store (false);
        readPosSnapshot.store (0.0);
    }

    if (producerDone.load())
        return false;

    const auto mode = sourceMode.load();
    const juce::ScopedLock srcLock (sourceLock);

    const auto& source = (mode == SourceMode::live) ? captureBuffer : fileBuffer;

    if (source.getNumSamples() <= 0)
        return false;

    const auto params = fetchParams();
    lastStretch.store ((double) params.stretch);

    // A seek keeps the overlap-add tail on purpose: the first frame at the new
    // position adds onto the decaying tail of the old one, so the splice
    // crossfades over half a window instead of stepping.
    const double seek = pendingSeek.exchange (-1.0);

    if (seek >= 0.0)
    {
        {
            const juce::SpinLock::ScopedLockType sl (fifoLock);
            fifo.reset();
        }

        stretcher.setReadPosition (seek * (double) source.getNumSamples());
        readPosSnapshot.store (stretcher.getReadPosition());
        producerDone.store (false);
        samplesConsumed.store (0);
    }

    const int hop = stretcher.getHopSize();

    if (hop <= 0 || fifo.getFreeSpace() < hop + 4)
        return false;

    // Stay only a little ahead of the audio thread. Running the FIFO all the
    // way full would buffer seconds of already-rendered audio and every
    // parameter change would arrive that late.
    if (fifo.getNumReady() >= targetBufferedSamples (hop))
        return false;

    auto* const* out = staging.getArrayOfWritePointers();

    // A window-size change means a different FFT and a different overlap-add
    // tail. Emitting the old tail first lands the previous frame on exactly
    // zero, so the switch is silent instead of a step discontinuity.
    const int wantedOrder = PaulStretch::orderForSeconds (params.windowSeconds, sampleRate);

    if (wantedOrder != stretcher.getOrder())
    {
        stretcher.flushTail (out, numChannels);
        pushHopToFifo (hop);

        const double keptPosition = stretcher.getReadPosition();
        stretcher.setOrder (wantedOrder);
        stretcher.setReadPosition (keptPosition);

        return true;
    }

    if (mode == SourceMode::live)
    {
        // Never read ahead of the write head, and never fall so far behind that
        // the ring has already overwritten what we are about to read.
        const auto capacity = (double) source.getNumSamples();
        const auto writeHead = (double) captureWrite.load();
        const auto window     = (double) stretcher.getWindowSize();

        const double newest = writeHead - window;
        const double oldest = writeHead - capacity + window;

        stretcher.setReadPosition (juce::jlimit (juce::jmin (oldest, newest),
                                                 newest,
                                                 stretcher.getReadPosition()));
    }
    else
    {
        const auto length = (double) source.getNumSamples();

        if (stretcher.getReadPosition() >= length)
        {
            if (looping.load())
            {
                stretcher.setReadPosition (std::fmod (stretcher.getReadPosition(), length));
            }
            else
            {
                stretcher.flushTail (out, numChannels);
                pushHopToFifo (hop);
                producerDone.store (true);
                return true;
            }
        }
    }

    stretcher.renderHop (source, onsets, out, numChannels, params);
    pushHopToFifo (hop);

    readPosSnapshot.store (stretcher.getReadPosition());

    {
        const juce::SpinLock::ScopedLockType sl (spectrumLock);
        spectrumSnapshot = stretcher.getDisplaySpectrum();
    }

    return true;
}

void StretchEngine::pushHopToFifo (int numSamples)
{
    // No lock here on purpose. AbstractFifo is safe for one producer and one
    // consumer running concurrently; the lock exists only to keep fifo.reset()
    // away from a read in progress, so taking it on every write would create
    // contention for no reason.
    int start1 = 0, size1 = 0, start2 = 0, size2 = 0;
    fifo.prepareToWrite (numSamples, start1, size1, start2, size2);

    for (int ch = 0; ch < numChannels; ++ch)
    {
        if (size1 > 0) fifoBuffer.copyFrom (ch, start1, staging, ch, 0,     size1);
        if (size2 > 0) fifoBuffer.copyFrom (ch, start2, staging, ch, size1, size2);
    }

    fifo.finishedWrite (size1 + size2);
}

//==============================================================================

void StretchEngine::pushInput (const juce::AudioBuffer<float>& input)
{
    if (sourceMode.load() != SourceMode::live)
        return;

    const int capacity = captureBuffer.getNumSamples();
    const int numIn    = input.getNumSamples();

    if (capacity <= 0 || numIn <= 0 || input.getNumChannels() <= 0)
        return;

    const long long head = captureWrite.load();
    int pos = (int) (head % capacity);
    int i   = 0;

    while (i < numIn)
    {
        const int chunk = juce::jmin (numIn - i, capacity - pos);

        for (int ch = 0; ch < captureBuffer.getNumChannels(); ++ch)
            captureBuffer.copyFrom (ch, pos, input, juce::jmin (ch, input.getNumChannels() - 1), i, chunk);

        i   += chunk;
        pos += chunk;

        if (pos >= capacity)
            pos = 0;
    }

    captureWrite.store (head + numIn);
}

int StretchEngine::readOutput (juce::AudioBuffer<float>& output)
{
    const int numSamples = output.getNumSamples();
    output.clear();

    if (! playing.load() || numSamples <= 0)
        return 0;

    // Try-lock only: the audio thread would rather emit one silent block than
    // wait on the render thread. The lock is contended only while the render
    // thread is resetting the FIFO, which happens on transport changes.
    const juce::SpinLock::ScopedTryLockType sl (fifoLock);

    if (! sl.isLocked())
        return 0;

    const int ready = juce::jmin (numSamples, fifo.getNumReady());

    if (ready > 0)
    {
        int start1 = 0, size1 = 0, start2 = 0, size2 = 0;
        fifo.prepareToRead (ready, start1, size1, start2, size2);

        for (int ch = 0; ch < output.getNumChannels(); ++ch)
        {
            const int src = juce::jmin (ch, numChannels - 1);

            if (size1 > 0) output.copyFrom (ch, 0,     fifoBuffer, src, start1, size1);
            if (size2 > 0) output.copyFrom (ch, size1, fifoBuffer, src, start2, size2);
        }

        fifo.finishedRead (size1 + size2);
        samplesConsumed.fetch_add (size1 + size2);
    }

    if (ready < numSamples)
        underruns.fetch_add (1);

    if (producerDone.load() && fifo.getNumReady() == 0)
        playing.store (false);

    return ready;
}

void StretchEngine::copyDisplaySpectrum (float* dest, int numValues) const
{
    const juce::SpinLock::ScopedTryLockType sl (spectrumLock);

    if (! sl.isLocked())
        return;

    const int n = juce::jmin (numValues, (int) spectrumSnapshot.size());

    for (int i = 0; i < n; ++i)
        dest[i] = spectrumSnapshot[(size_t) i];
}

//==============================================================================

juce::String StretchEngine::renderToFile (const juce::File& destination,
                                          double maxSeconds,
                                          const StretchParams& params,
                                          std::function<bool (double)> onProgress)
{
    juce::AudioBuffer<float> source;
    std::vector<Onset>       sourceOnsets;

    {
        const juce::ScopedLock sl (sourceLock);

        if (fileBuffer.getNumSamples() <= 0)
            return "Nothing loaded to render.";

        source.makeCopyOf (fileBuffer);
        sourceOnsets = onsets;
    }

    // Its own stretcher, so exporting does not disturb what is playing.
    PaulStretch offline;
    offline.prepare (sampleRate, numChannels);
    offline.setOrder (PaulStretch::orderForSeconds (params.windowSeconds, sampleRate));

    const int hop = offline.getHopSize();

    const double stretched  = (double) source.getNumSamples() * (double) juce::jmax (1.0f, params.stretch);
    const auto totalSamples = (long long) juce::jmin (stretched, juce::jmax (1.0, maxSeconds) * sampleRate);

    destination.deleteFile();

    std::unique_ptr<juce::OutputStream> stream (destination.createOutputStream());

    if (stream == nullptr)
        return "Could not write to " + destination.getFullPathName();

    juce::WavAudioFormat wav;

    auto writer = wav.createWriterFor (stream, juce::AudioFormatWriterOptions {}
                                                   .withSampleRate (sampleRate)
                                                   .withNumChannels (numChannels)
                                                   .withBitsPerSample (24));

    if (writer == nullptr)
        return "Could not create a WAV writer.";

    juce::AudioBuffer<float> block (numChannels, hop);
    long long written = 0;

    while (written < totalSamples)
    {
        offline.renderHop (source, sourceOnsets, block.getArrayOfWritePointers(), numChannels, params);

        const auto toWrite = (int) juce::jmin ((long long) hop, totalSamples - written);

        if (! writer->writeFromAudioSampleBuffer (block, 0, toWrite))
        {
            writer.reset();
            return "Ran out of disk space while writing.";
        }

        written += toWrite;

        if (onProgress != nullptr && ! onProgress ((double) written / (double) totalSamples))
        {
            writer.reset();
            destination.deleteFile();
            return "Cancelled.";
        }
    }

    writer.reset();
    return {};
}

} // namespace es
