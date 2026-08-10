#include <tt/match.hpp>

#include <algorithm>
#include <cstddef>
#include <vector>

namespace tt {
namespace {

bool is_ascii_alnum(unsigned char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9');
}

// Short on purpose: anything plausibly part of a real client or matter name stays off.
bool is_filler(const std::string& token) {
    static constexpr std::string_view kFillers[] = {
        "a", "an", "and", "at", "for", "in", "it", "its", "my", "of", "on",
        "or", "our", "please", "stuff", "that", "the", "these", "thing",
        "things", "this", "those", "to", "uh", "um", "with",
    };
    return std::find(std::begin(kFillers), std::end(kFillers), token) != std::end(kFillers);
}

}

std::string normalize(std::string_view text) {
    // Also defines the `name_key` column
    std::string out;
    out.reserve(text.size());
    for (const char raw : text) {
        const unsigned char c = static_cast<unsigned char>(raw);
        if (c >= 0x80) {
            out.push_back(raw);  // leave UTF-8 alone, accented names survive
        } else if (is_ascii_alnum(c)) {
            out.push_back(static_cast<char>(c >= 'A' && c <= 'Z' ? c - 'A' + 'a' : c));
        } else if (!out.empty() && out.back() != ' ') {
            out.push_back(' ');  // only ever one space in a row
        }
    }
    if (!out.empty() && out.back() == ' ') {
        out.pop_back();
    }
    return out;
}

std::vector<std::string> tokenize(std::string_view text) {
    const std::string s = normalize(text);
    std::vector<std::string> tokens;
    std::size_t start = 0;
    while (start < s.size()) {
        const std::size_t space = s.find(' ', start);
        if (space == std::string::npos) {
            tokens.push_back(s.substr(start));
            break;
        }
        tokens.push_back(s.substr(start, space - start));
        start = space + 1;
    }
    return tokens;
}

bool exact_match(std::string_view a, std::string_view b) { return normalize(a) == normalize(b); }

int levenshtein(std::string_view a, std::string_view b) {
    const int n = static_cast<int>(a.size());
    const int m = static_cast<int>(b.size());

    std::vector<int> prev(static_cast<std::size_t>(m) + 1);
    std::vector<int> cur(static_cast<std::size_t>(m) + 1);
    for (int j = 0; j <= m; ++j) {
        prev[static_cast<std::size_t>(j)] = j;
    }

    for (int i = 1; i <= n; ++i) {
        cur[0] = i;
        for (int j = 1; j <= m; ++j) {
            const std::size_t u = static_cast<std::size_t>(j);
            const int cost = a[static_cast<std::size_t>(i - 1)] == b[u - 1] ? 0 : 1;
            cur[u] = std::min({prev[u] + 1, cur[u - 1] + 1, prev[u - 1] + cost});
        }
        prev.swap(cur);
    }
    return prev[static_cast<std::size_t>(m)];
}

double token_score(std::string_view query, std::string_view candidate) {
    if (query.empty() || candidate.empty()) {
        return 0.0;
    }
    if (query == candidate) {
        return 1.0;
    }

    const double longer_len = static_cast<double>(std::max(query.size(), candidate.size()));

    double best = 0.0;
    // Prefix bonus one way only: "north" reaches "Northgate", never the reverse
    if (query.size() >= kMinPrefixLength && candidate.starts_with(query)) {
        best = kPrefixBonus + kPrefixCoverage * (static_cast<double>(query.size()) / longer_len);
    }

    const int distance = levenshtein(query, candidate);
    const double edit_score = 1.0 - static_cast<double>(distance) / longer_len;
    return std::max(best, std::max(edit_score, 0.0));
}

double phrase_score(std::string_view query, std::string_view candidate) {
    const std::vector<std::string> all = tokenize(query);
    const std::vector<std::string> c = tokenize(candidate);
    if (all.empty() || c.empty()) {
        return 0.0;
    }

    // Filler stripped from the query, not the candidate. An all-filler query keeps its tokens.
    std::vector<std::string> q;
    for (const std::string& t : all) {
        if (!is_filler(t)) {
            q.push_back(t);
        }
    }
    if (q.empty()) {
        q = all;
    }

    double total = 0.0;
    for (const std::string& qt : q) {
        double best = 0.0;
        for (const std::string& ct : c) {
            best = std::max(best, token_score(qt, ct));
        }
        // Every query token has to land somewhere
        if (best < kMinTokenScore) {
            return 0.0;
        }
        total += best;
    }

    // Coverage of the query, not equality => exact_match() is a separate whole-string test
    return total / static_cast<double>(q.size());
}

}
