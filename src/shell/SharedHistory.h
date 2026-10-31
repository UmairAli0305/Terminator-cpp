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
#include <semaphore.h>
#include <sys/mman.h>
#include <cstring>
#include <new>
#include <cstdio>

// One mmap-backed history shared by all tabs and inherited across fork().
// The file remains the persistent store across Terminator restarts.
namespace SharedHistory {
// Fixed-size shared buffer. Keep memory bounded and avoid pointers inside mmap.
constexpr size_t kCapacity = 32 * 1024 * 1024;
struct SharedRegion {
    sem_t lock;
    size_t length;
    char bytes[kCapacity];
};
inline SharedRegion* region = nullptr;
#ifdef __APPLE__
// macOS does not implement unnamed process-shared POSIX semaphores.
// Create a unique named semaphore before fork(); children inherit the handle.
inline sem_t* namedLock = SEM_FAILED;
#endif
inline sem_t* semaphore() {
#ifdef __APPLE__
    return namedLock;
#else
    return region ? &region->lock : nullptr;
#endif
}

inline bool lockRegion() {
    sem_t* lock = semaphore();
    if (!region || !lock || lock == SEM_FAILED) return false;
    while (sem_wait(lock) == -1) {
        if (errno != EINTR) return false;
    }
    return true;
}
inline void unlockRegion() { if (region && semaphore()) sem_post(semaphore()); }

inline std::vector<std::string> parse(const std::string& content) {
    std::vector<std::string> result;
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
inline void updateRegion(const std::vector<std::string>& entries) {
    if (!region) return;
    std::string data;
    // Retain newest commands when history exceeds the fixed shared-memory budget.
    for (auto it = entries.rbegin(); it != entries.rend(); ++it) {
        if (it->size() + 1 > kCapacity - data.size()) break;
        data.insert(0, *it + "\n");
    }
    std::memcpy(region->bytes, data.data(), data.size());
    region->length = data.size();
}

inline std::string path() {
    const char* home = std::getenv("HOME");
    return std::string(home && *home ? home : ".") + "/.terminator_history";
}
inline std::vector<std::string> loadFromDisk() {
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
    return parse(content);
}
inline bool initialize() {
    if (region) return true;
    void* mem = mmap(nullptr, sizeof(SharedRegion), PROT_READ | PROT_WRITE,
                     MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    if (mem == MAP_FAILED) return false;
    auto* shared = static_cast<SharedRegion*>(mem);
    shared->length = 0;
#ifdef __APPLE__
    // Unlink immediately: the open handle remains usable by forked children,
    // and an abnormal exit cannot leave a stale named semaphore behind.
    char name[96];
    std::snprintf(name, sizeof(name), "/terminator-history-%ld", static_cast<long>(getpid()));
    namedLock = sem_open(name, O_CREAT | O_EXCL, 0600, 1);
    if (namedLock == SEM_FAILED) {
        munmap(mem, sizeof(SharedRegion));
        return false;
    }
    sem_unlink(name);
#else
    if (sem_init(&shared->lock, 1, 1) != 0) {
        munmap(mem, sizeof(SharedRegion));
        return false;
    }
#endif
    region = shared;
    updateRegion(loadFromDisk());
    return true;
}
inline void shutdown() {
    if (!region) return;
#ifdef __APPLE__
    if (namedLock != SEM_FAILED) {
        sem_close(namedLock);
        namedLock = SEM_FAILED;
    }
#else
    sem_destroy(&region->lock); // Call only in the parent, after all tabs have stopped.
#endif
    munmap(region, sizeof(SharedRegion));
    region = nullptr;
}
inline std::vector<std::string> load() {
    if (!region) return loadFromDisk();
    if (!lockRegion()) return {};
    std::string data(region->bytes, region->length);
    unlockRegion();
    return parse(data);
}
inline bool append(const std::string& command) {
    if (command.empty()) return true;
    const bool locked = lockRegion();
    if (region && !locked) return false;
    int fd = open(path().c_str(), O_RDWR | O_CREAT, 0600);
    if (fd < 0) { if (locked) unlockRegion(); return false; }
    if (flock(fd, LOCK_EX) != 0) { close(fd); if (locked) unlockRegion(); return false; }
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
    if (ok && locked) updateRegion(entries);
    flock(fd, LOCK_UN);
    close(fd);
    if (locked) unlockRegion();
    return ok;
}
inline std::vector<std::string> recent(size_t count = 1000) {
    auto entries = load();
    if (entries.size() > count) entries.erase(entries.begin(), entries.end() - count);
    return entries;
}
}
#endif
