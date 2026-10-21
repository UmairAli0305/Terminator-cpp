#ifndef FILENAME_COMPLETION_H
#define FILENAME_COMPLETION_H

#include <algorithm>
#include <filesystem>
#include <string>
#include <system_error>
#include <vector>

namespace FilenameCompletion {
struct Result {
    bool changed = false;
    bool ambiguous = false;
    std::vector<std::string> matches;
    std::string directory;
};

inline Result complete(std::string& input, size_t& cursor,
                       const std::filesystem::path& cwd) {
    Result result;
    cursor = std::min(cursor, input.size());
    // Complete only the word immediately preceding the cursor.
    size_t begin = cursor;
    while (begin > 0 && input[begin - 1] != ' ' &&
           input[begin - 1] != '\t' && input[begin - 1] != '\n') --begin;
    const std::string word = input.substr(begin, cursor - begin);
    if (word.empty()) return result;
    const size_t slash = word.find_last_of('/');
    const std::string directory = slash == std::string::npos ? "" : word.substr(0, slash + 1);
    const std::string prefix = slash == std::string::npos ? word : word.substr(slash + 1);
    result.directory = directory;
    std::filesystem::path folder = directory.empty() ? cwd : std::filesystem::path(directory);
    if (folder.is_relative()) folder = cwd / folder;
    std::error_code ec;
    std::filesystem::directory_iterator it(folder, ec), end;
    if (ec) return result;
    for (; it != end; it.increment(ec)) {
        if (ec) break;
        const std::string name = it->path().filename().string();
        if (name.compare(0, prefix.size(), prefix) == 0)
            result.matches.push_back(name);
    }
    if (result.matches.empty()) return result;
    std::sort(result.matches.begin(), result.matches.end());
    std::string replacement = result.matches.front();
    if (result.matches.size() > 1) {
        for (size_t i = 1; i < result.matches.size(); ++i) {
            size_t j = 0;
            while (j < replacement.size() && j < result.matches[i].size() &&
                   replacement[j] == result.matches[i][j]) ++j;
            replacement.resize(j);
        }
        result.ambiguous = true;
    }
    if (replacement.size() > prefix.size()) {
        input.replace(begin, cursor - begin, directory + replacement);
        cursor = begin + directory.size() + replacement.size();
        result.changed = true;
    }
    return result;
}
// Replace the word before the cursor, preserving all text after the cursor.
inline void choose(std::string& input, size_t& cursor,
                   const std::string& directory, const std::string& filename) {
    cursor = std::min(cursor, input.size());
    size_t begin = cursor;
    while (begin > 0 && input[begin - 1] != ' ' &&
           input[begin - 1] != '\t' && input[begin - 1] != '\n') --begin;
    const std::string replacement = directory + filename;
    input.replace(begin, cursor - begin, replacement);
    cursor = begin + replacement.size();
}
} // namespace FilenameCompletion
#endif
