#ifndef TERMINATOR_SHARED_HISTORY_H
#define TERMINATOR_SHARED_HISTORY_H

#include <algorithm>
#include <cerrno>
#include <cstdlib>
#include <string>
#include <vector>
#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>

// All tabs and both execution modes use the same on-disk history.
// flock protects read/modify/write across independent tab processes.
namespace SharedHistory {
inline std::string path() {
    const char* home = std::getenv("HOME");
    return std::string(home && *home ? home : ".") + "/.terminator_history";
}
inline std::vector<std::string> load() {
    std::vector<std::string> result;
    int fd = open(path().c_str(), O_RDONLY | O_CREAT, 0600);
    if (fd < 0) return result;
    if (flock(fd, LOCK_SH) != 0) { close(fd); return result; }
    std::string content;
    char buffer[8192];
    ssize_t n;
    while ((n = read(fd, buffer, sizeof(buffer))) > 0)
        content.append(buffer, static_cast<size_t>(n));
    flock(fd, LOCK_UN);
    close(fd);
    size_t begin = 0;
    while (begin < content.size()) {
        size_t end = content.find('\n', begin);
        if (end == std::string::npos) end = content.size();
        if (end > begin) result.push_back(content.substr(begin, end - begin));
        begin = end + 1;
    }
    if (result.size() > 10000) result.erase(result.begin(), result.end() - 10000);
    return result;
}
inline bool append(const std::string& command) {
    if (command.empty()) return true;
    int fd = open(path().c_str(), O_RDWR | O_CREAT, 0600);
    if (fd < 0) return false;
    if (flock(fd, LOCK_EX) != 0) { close(fd); return false; }
    std::string content;
    char buffer[8192];
    ssize_t n;
    while ((n = read(fd, buffer, sizeof(buffer))) > 0)
        content.append(buffer, static_cast<size_t>(n));
    std::vector<std::string> entries;
    size_t begin = 0;
    while (begin < content.size()) {
        size_t end = content.find('\n', begin);
        if (end == std::string::npos) end = content.size();
        if (end > begin) entries.push_back(content.substr(begin, end - begin));
        begin = end + 1;
    }
    // Multiline commands are stored as one escaped line.
    std::string single = command;
    std::replace(single.begin(), single.end(), '\n', ' ');
    entries.push_back(single);
    if (entries.size() > 10000) entries.erase(entries.begin(), entries.end() - 10000);
    std::string output;
    for (const auto& entry : entries) output += entry + "\n";
    bool ok = lseek(fd, 0, SEEK_SET) >= 0 && ftruncate(fd, 0) == 0;
    size_t offset = 0;
    while (ok && offset < output.size()) {
        ssize_t written = write(fd, output.data() + offset, output.size() - offset);
        if (written < 0 && errno == EINTR) continue;
        if (written <= 0) { ok = false; break; }
        offset += static_cast<size_t>(written);
    }
    flock(fd, LOCK_UN);
    close(fd);
    return ok;
}
inline std::vector<std::string> recent(size_t count = 1000) {
    auto entries = load();
    if (entries.size() > count) entries.erase(entries.begin(), entries.end() - count);
    return entries;
}
}
#endif
