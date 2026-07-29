#pragma once

#include <juce_core/juce_core.h>

namespace es::duration
{

/** Human-readable length, from milliseconds up to "9.51 billion years".

    At a stretch ratio of 10^15 the output length of an ordinary track runs past
    the age of the universe, and reading that number off the screen is half the
    point of the thing — so this does not give up and print scientific notation
    until it genuinely has to. */
juce::String format (double seconds);

/** Same, but always short enough for a status line. */
juce::String formatShort (double seconds);

/** Parses a target length. Understands:
      "90"  "90s"  "2.5 min"  "3h"  "4 days"  "1:30"  "2:15:00"
      "1000 years"  "9.5 billion years"  "3 million years"
    Returns a negative value if the text makes no sense. */
double parse (juce::String text);

} // namespace es::duration
