/*
    Headless renderer — paulstretch from the command line, using the same
    engine the plugin runs.

        extremestretch-render in.wav out.wav [--stretch N] [--window S]
                              [--pitch S] [--blur 0..1] [--rot 0..1]
                              [--scramble 0..1] [--lowcut Hz] [--highcut Hz]
                              [--width 0..1] [--max-seconds S]
*/

#include "../Source/StretchEngine.h"
#include "../Source/Duration.h"

#include <juce_events/juce_events.h>

#include <cstdio>
#include <unistd.h>

namespace
{

double numberAfter (const juce::StringArray& args, const juce::String& flag, double fallback)
{
    const int index = args.indexOf (flag);

    return (index >= 0 && index + 1 < args.size()) ? args[index + 1].getDoubleValue()
                                                   : fallback;
}

juce::String textAfter (const juce::StringArray& args, const juce::String& flag)
{
    const int index = args.indexOf (flag);
    return (index >= 0 && index + 1 < args.size()) ? args[index + 1] : juce::String();
}

void printUsage()
{
    std::puts (
        "extremestretch-render — Paul's Extreme Sound Stretch, offline\n"
        "\n"
        "  extremestretch-render <in> <out.wav> [options]\n"
        "\n"
        "  --stretch N       time stretch ratio, 1 .. 1e15  (default 8)\n"
        "  --length T        target output length; sets --stretch to match.\n"
        "                    \"3h\", \"45 min\", \"2:30\", \"1000 years\", \"9.5 billion years\"\n"
        "  --algorithm A     smooth | onset | vocoder | grain | hold  (default smooth)\n"
        "  --shape 0..1      meaning depends on the algorithm  (default 0.5)\n"
        "                    onset=sensitivity vocoder=coherence grain=spray hold=hold\n"
        "  --window S        analysis window in seconds     (default 0.25)\n"
        "  --pitch S         pitch shift in semitones       (default 0)\n"
        "  --blur 0..1       spectral smear                 (default 0)\n"
        "  --rot 0..1        progressive bin death          (default 0)\n"
        "  --scramble 0..1   local bin displacement         (default 0)\n"
        "  --lowcut Hz       spectral high-pass             (default 20)\n"
        "  --highcut Hz      spectral low-pass              (default 20000)\n"
        "  --width 0..1      channel decorrelation          (default 1)\n"
        "  --max-seconds S   cap on what is actually written (default 3600)\n");
}

es::Algorithm parseAlgorithm (const juce::String& name, bool& ok)
{
    ok = true;

    for (int i = 0; i < es::numAlgorithms; ++i)
        if (name.equalsIgnoreCase (es::toString ((es::Algorithm) i)))
            return (es::Algorithm) i;

    ok = name.isEmpty();
    return es::Algorithm::smooth;
}

} // namespace

int main (int argc, char* argv[])
{
    juce::ScopedJuceInitialiser_GUI juceInit;

    juce::StringArray args;

    for (int i = 1; i < argc; ++i)
        args.add (juce::String (argv[i]));

    // Positional arguments only — every flag here takes exactly one value, so
    // skipping the token after a "--" leaves just the two filenames.
    juce::StringArray files;

    for (int i = 0; i < args.size(); ++i)
    {
        if (args[i].startsWith ("--"))
        {
            ++i; // skip its value
            continue;
        }

        files.add (args[i]);
    }

    if (files.size() < 2)
    {
        printUsage();
        return files.isEmpty() ? 0 : 1;
    }

    const juce::File input  (juce::File::getCurrentWorkingDirectory().getChildFile (files[0]));
    const juce::File output (juce::File::getCurrentWorkingDirectory().getChildFile (files[1]));

    es::StretchEngine engine;
    engine.prepare (44100.0, 2);

    const auto loadError = engine.loadFile (input);

    if (loadError.isNotEmpty())
    {
        std::printf ("error: %s\n", loadError.toRawUTF8());
        return 1;
    }

    bool algorithmOk = true;
    const auto algorithm = parseAlgorithm (textAfter (args, "--algorithm"), algorithmOk);

    if (! algorithmOk)
    {
        std::printf ("error: unknown algorithm '%s'\n", textAfter (args, "--algorithm").toRawUTF8());
        return 1;
    }

    es::StretchParams p;
    p.algorithm      = algorithm;
    p.shape          = (float) numberAfter (args, "--shape",    0.5);
    p.stretch        = (float) numberAfter (args, "--stretch",  8.0);
    p.windowSeconds  = (float) numberAfter (args, "--window",   0.25);
    p.pitchSemitones = (float) numberAfter (args, "--pitch",    0.0);
    p.blur           = (float) numberAfter (args, "--blur",     0.0);
    p.rot            = (float) numberAfter (args, "--rot",      0.0);
    p.scramble       = (float) numberAfter (args, "--scramble", 0.0);
    p.lowCutHz       = (float) numberAfter (args, "--lowcut",   20.0);
    p.highCutHz      = (float) numberAfter (args, "--highcut",  20000.0);
    p.width          = (float) numberAfter (args, "--width",    1.0);

    const double maxSeconds = numberAfter (args, "--max-seconds", 3600.0);

    // --length overrides --stretch: work out the ratio the wanted duration needs.
    const auto lengthText = textAfter (args, "--length");

    if (lengthText.isNotEmpty())
    {
        const double wanted = es::duration::parse (lengthText);

        if (wanted <= 0.0)
        {
            std::printf ("error: could not read the length '%s'\n", lengthText.toRawUTF8());
            return 1;
        }

        p.stretch = (float) juce::jlimit (1.0, 1.0e15, wanted / engine.getSourceSeconds());
    }

    const double total = engine.getSourceSeconds() * (double) p.stretch;

    std::printf ("in    %s   %s\n", input.getFileName().toRawUTF8(),
                 es::duration::format (engine.getSourceSeconds()).toRawUTF8());
    std::printf ("out   %s\n", output.getFileName().toRawUTF8());
    std::printf ("      %s, shape %.0f%%, window %.0f ms, stretch %.6g x\n",
                 es::toString (p.algorithm), p.shape * 100.0f, p.windowSeconds * 1000.0, p.stretch);
    std::printf ("      full length %s", es::duration::format (total).toRawUTF8());

    if (total > maxSeconds)
        std::printf ("  (writing the first %s)", es::duration::format (maxSeconds).toRawUTF8());

    std::puts ("");

    int        lastPercent = -1;
    const bool interactive = isatty (fileno (stdout)) != 0;

    const auto error = engine.renderToFile (output, maxSeconds, p,
                                            [&lastPercent, interactive] (double progress)
                                            {
                                                const int percent = (int) (progress * 100.0);
                                                const int step    = interactive ? 1 : 25;

                                                if (percent / step != lastPercent / step)
                                                {
                                                    lastPercent = percent;
                                                    std::printf (interactive ? "\r      %3d%%" : "      %3d%%\n", percent);
                                                    std::fflush (stdout);
                                                }

                                                return true;
                                            });

    std::puts ("");

    if (error.isNotEmpty())
    {
        std::printf ("error: %s\n", error.toRawUTF8());
        return 1;
    }

    std::printf ("done  %s\n", juce::File::descriptionOfSizeInBytes (output.getSize()).toRawUTF8());
    return 0;
}
