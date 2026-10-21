#ifndef TERMINATOR_HISTORY_SEARCH_H
#define TERMINATOR_HISTORY_SEARCH_H

#include "SharedHistory.h"
#include <algorithm>
#include <string>
#include <vector>

namespace HistorySearch {
// Return the newest exact match, otherwise every command tied for the
// longest common CONTIGUOUS substring (at least three characters).
inline std::vector<std::string> find(const std::string& term) {
    const auto history = SharedHistory::load();
    for (auto it = history.rbegin(); it != history.rend(); ++it)
        if (*it == term) return {*it};
    if (term.empty()) return {};
    size_t best = 2;
    std::vector<std::string> matches;
    for (auto it = history.rbegin(); it != history.rend(); ++it) {
        const std::string& cmd = *it;
        std::vector<size_t> previous(term.size() + 1, 0);
        size_t longest = 0;
        for (char c : cmd) {
            std::vector<size_t> current(term.size() + 1, 0);
            for (size_t j = 1; j <= term.size(); ++j)
                if (c == term[j - 1]) {
                    current[j] = previous[j - 1] + 1;
                    longest = std::max(longest, current[j]);
                }
            previous.swap(current);
        }
        if (longest > best) { best = longest; matches.clear(); matches.push_back(cmd); }
        else if (longest == best && longest >= 3) matches.push_back(cmd);
    }
    return matches;
}
} // namespace HistorySearch
#endif
