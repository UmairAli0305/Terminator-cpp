#ifndef TERMINATOR_MULTILINE_INPUT_H
#define TERMINATOR_MULTILINE_INPUT_H

#include <string>

namespace MultilineInput {
// Stage 1: detect incomplete single/double quotes and trailing backslash.
// Command substitution nesting is implemented in a later parser stage.
inline bool needsMore(const std::string& command) {
    char quote = 0;
    bool escaped = false;
    int substitutions = 0;
    bool backtick = false;
    for (size_t i = 0; i < command.size(); ++i) {
        char c = command[i];
        if (escaped) { escaped = false; continue; }
        if (c == '\\' && quote != '\'') { escaped = true; continue; }
        if (quote == '\'') { if (c == '\'') quote = 0; continue; }
        if (c == '\'') { quote = '\''; continue; }
        if (c == '`') { backtick = !backtick; continue; }
        if (c == '"') { quote = quote == '"' ? 0 : '"'; continue; }
        if (c == '$' && i + 1 < command.size() && command[i+1] == '(') {
            ++substitutions; ++i; continue;
        }
        if (c == ')' && substitutions > 0) --substitutions;
    }
    return quote != 0 || escaped || substitutions != 0 || backtick;
}
// A backslash immediately before Enter removes the newline.
inline void appendLine(std::string& pending, const std::string& line) {
    if (!pending.empty() && pending.back() == '\\') {
        pending.pop_back();
        pending += line;
    } else {
        if (!pending.empty()) pending += '\n';
        pending += line;
    }
}
}
#endif
