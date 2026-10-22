#ifndef SCRIPT_VARIABLES_H
#define SCRIPT_VARIABLES_H

#include <cctype>
#include <map>
#include <string>
#include <vector>

// Script variable expansion happens in Terminator, never in /bin/sh.
namespace ScriptVariables {
inline bool identifierStart(char c) {
    return std::isalpha(static_cast<unsigned char>(c)) || c == '_';
}
inline bool identifierChar(char c) {
    return std::isalnum(static_cast<unsigned char>(c)) || c == '_';
}
inline bool assignment(const std::string& line, std::string& name, std::string& value) {
    size_t first = line.find_first_not_of(" \t");
    if (first == std::string::npos || !identifierStart(line[first])) return false;
    size_t i = first + 1;
    while (i < line.size() && identifierChar(line[i])) ++i;
    if (i == line.size() || line[i] != '=') return false;
    name = line.substr(first, i - first);
    value = line.substr(i + 1);
    return true;
}
inline std::string expand(const std::string& input,
                          const std::map<std::string, std::string>& variables,
                          const std::vector<std::string>& args, int status) {
    std::string out;
    char quote = 0;
    for (size_t i = 0; i < input.size(); ++i) {
        char c = input[i];
        if (c == '\\' && quote != '\'' && i + 1 < input.size()) {
            out += c;
            out += input[++i];
            continue;
        }
        if (c == '\'' && quote != '"') { quote = quote == '\'' ? 0 : '\''; out += c; continue; }
        if (c == '"' && quote != '\'') { quote = quote == '"' ? 0 : '"'; out += c; continue; }
        if (c != '$' || quote == '\'' || i + 1 >= input.size()) { out += c; continue; }
        char next = input[i + 1];
        if (next == '?') { out += std::to_string(status); ++i; continue; }
        if (next == '#') { out += std::to_string(args.size()); ++i; continue; }
        if (next >= '0' && next <= '9') {
            size_t index = static_cast<size_t>(next - '0');
            if (index > 0 && index <= args.size()) out += args[index - 1];
            ++i;
            continue;
        }
        if (next == '{') {
            size_t end = input.find('}', i + 2);
            if (end != std::string::npos) {
                std::string key = input.substr(i + 2, end - i - 2);
                auto it = variables.find(key);
                if (it != variables.end()) out += it->second;
                i = end;
                continue;
            }
        }
        if (identifierStart(next)) {
            size_t end = i + 2;
            while (end < input.size() && identifierChar(input[end])) ++end;
            auto it = variables.find(input.substr(i + 1, end - i - 1));
            if (it != variables.end()) out += it->second;
            i = end - 1;
            continue;
        }
        out += c;
    }
    return out;
}
}
#endif
