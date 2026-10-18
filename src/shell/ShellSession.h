
#ifndef SHELL_SESSION_H
#define SHELL_SESSION_H

#include <filesystem>
#include <string>
#include <vector>

class ShellSession {
private:
    std::filesystem::path cwd;
    int lastStatus = 0;

public:
    ShellSession();

    const std::filesystem::path& getCwd() const;
    int getLastStatus() const;
    void setLastStatus(int status);

    bool changeDirectory(
        const std::string& destination,
        std::string& error
    );

    std::string expandExitStatus(
        const std::string& command
    ) const;
};

#endif
