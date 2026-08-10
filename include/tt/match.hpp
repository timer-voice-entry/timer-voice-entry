#pragma once

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace tt
{

    inline constexpr double kMinTokenScore = 0.60;

    inline constexpr double kMinPhraseScore = 0.55;

    inline constexpr double kDescriptionWeight = 0.70;

    // What a prefix match scores before the length share
    inline constexpr double kPrefixBonus = 0.75;
    inline constexpr double kPrefixCoverage = 0.20;

    inline constexpr std::size_t kMinPrefixLength = 3;

    std::string normalize(std::string_view text);

    // normalize(), then split on the single spaces it leaves behind.
    std::vector<std::string> tokenize(std::string_view text);

    bool exact_match(std::string_view a, std::string_view b);

    int levenshtein(std::string_view a, std::string_view b);

    double token_score(std::string_view query, std::string_view candidate);

    double phrase_score(std::string_view query, std::string_view candidate);

}
