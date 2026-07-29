#include "Duration.h"

#include <cmath>

namespace es::duration
{

namespace
{
    constexpr double minute = 60.0;
    constexpr double hour   = 60.0 * minute;
    constexpr double day    = 24.0 * hour;
    constexpr double year   = 365.25 * day;

    juce::String withSeparators (double value, int decimals)
    {
        // juce::String (double, 0) does NOT mean "no decimals" — it means
        // shortest round-trip, which happily prints 1000.08. Round explicitly.
        auto text = decimals > 0 ? juce::String (value, decimals)
                                 : juce::String ((juce::int64) std::llround (value));

        const int dot   = text.indexOfChar ('.');
        int       start = dot >= 0 ? dot : text.length();

        // Walk left from the decimal point inserting a thin separator.
        for (int i = start - 3; i > 0; i -= 3)
            text = text.substring (0, i) + "," + text.substring (i);

        return text;
    }

    juce::String clock (double seconds, bool forceHours)
    {
        const auto total = (juce::int64) seconds;
        const auto h     = total / 3600;
        const auto m     = (total / 60) % 60;
        const auto s     = total % 60;

        if (h > 0 || forceHours)
            return juce::String (h) + ":" + juce::String (m).paddedLeft ('0', 2)
                 + ":" + juce::String (s).paddedLeft ('0', 2);

        return juce::String (m) + ":" + juce::String (s).paddedLeft ('0', 2);
    }
}

juce::String format (double seconds)
{
    if (! std::isfinite (seconds) || seconds < 0.0)
        return "-";

    if (seconds < 0.001)              return "0 s";
    if (seconds < 10.0)               return juce::String (seconds, 2) + " s";
    if (seconds < minute)             return juce::String (seconds, 1) + " s";
    if (seconds < day)                return clock (seconds, seconds >= hour);

    if (seconds < 60.0 * day)
    {
        const double days = seconds / day;
        return juce::String (days, days < 10.0 ? 1 : 0) + (days < 2.0 ? " day" : " days");
    }

    const double years = seconds / year;

    if (years < 1.0)                  return juce::String (seconds / day, 0) + " days";
    if (years < 1000.0)               return juce::String (years, years < 10.0 ? 2 : 1) + " years";
    if (years < 1.0e6)                return withSeparators (years, 0) + " years";
    if (years < 1.0e9)                return juce::String (years / 1.0e6, 2) + " million years";
    if (years < 1.0e12)               return juce::String (years / 1.0e9, 2) + " billion years";
    if (years < 1.0e15)               return juce::String (years / 1.0e12, 2) + " trillion years";
    if (years < 1.0e18)               return juce::String (years / 1.0e15, 2) + " quadrillion years";

    return juce::String (years, 3) + " years";
}

juce::String formatShort (double seconds)
{
    auto text = format (seconds);

    return text.replace (" quadrillion years", " Qyr")
               .replace (" trillion years",    " Tyr")
               .replace (" billion years",     " Gyr")
               .replace (" million years",     " Myr")
               .replace (" years",             " yr")
               .replace (" days",              " d")
               .replace (" day",               " d");
}

double parse (juce::String text)
{
    // Accept anything format() can produce, thousands separators included.
    text = text.removeCharacters (",").trim().toLowerCase();

    if (text.isEmpty())
        return -1.0;

    // clock forms first: 2:15 or 1:02:30
    if (text.containsChar (':') && ! text.containsChar (' '))
    {
        auto parts = juce::StringArray::fromTokens (text, ":", {});

        if (parts.size() == 2)
            return parts[0].getDoubleValue() * minute + parts[1].getDoubleValue();

        if (parts.size() == 3)
            return parts[0].getDoubleValue() * hour + parts[1].getDoubleValue() * minute
                 + parts[2].getDoubleValue();

        return -1.0;
    }

    // leading number
    int i = 0;

    while (i < text.length()
           && (juce::CharacterFunctions::isDigit (text[i]) || text[i] == '.'
               || text[i] == '-' || text[i] == '+' || text[i] == 'e'))
        ++i;

    if (i == 0)
        return -1.0;

    double value = text.substring (0, i).getDoubleValue();
    auto   rest  = text.substring (i).trim();

    // scale words
    struct Scale { const char* word; double factor; };

    for (const auto& s : { Scale { "thousand",    1.0e3 },
                           Scale { "million",     1.0e6 },
                           Scale { "billion",     1.0e9 },
                           Scale { "trillion",    1.0e12 },
                           Scale { "quadrillion", 1.0e15 } })
    {
        if (rest.startsWith (s.word))
        {
            value *= s.factor;
            rest = rest.substring ((int) juce::String (s.word).length()).trim();
            break;
        }
    }

    if (rest.isEmpty())
        return value;   // bare number means seconds

    struct Unit { const char* word; double factor; };

    // Longest first so "min" is not eaten by "m", and "months" not by "min".
    for (const auto& u : { Unit { "millenni", 1000.0 * year },
                           Unit { "centur",   100.0 * year },
                           Unit { "year",     year },
                           Unit { "yr",       year },
                           Unit { "month",    year / 12.0 },
                           Unit { "week",     7.0 * day },
                           Unit { "day",      day },
                           Unit { "hour",     hour },
                           Unit { "hr",       hour },
                           Unit { "minute",   minute },
                           Unit { "min",      minute },
                           Unit { "second",   1.0 },
                           Unit { "sec",      1.0 },
                           Unit { "y",        year },
                           Unit { "w",        7.0 * day },
                           Unit { "d",        day },
                           Unit { "h",        hour },
                           Unit { "m",        minute },
                           Unit { "s",        1.0 } })
    {
        if (rest.startsWith (u.word))
            return value * u.factor;
    }

    return -1.0;
}

} // namespace es::duration
