
#include "ShellSession.h"

#include <cstdlib>
#include <system_error>

ShellSession::ShellSession()
    : cwd(std::filesystem::current_path()) {}

const std::filesystem::path& ShellSession::getCwd() const {
    return cwd;
}

int ShellSession::getLastStatus() const {
    return lastStatus;
}

void ShellSession::setLastStatus(int status) {
    lastStatus = status;
}

bool ShellSession::changeDirectory(
    const std::string& destination,
    std::string& error
) {
    std::string path = destination;

    if (path.empty() || path == "~" ||
        path.rfind("~/", 0) == 0) {

        const char* home = std::getenv("HOME");

        if (!home) {
            error = "cd: HOME is not set";
            lastStatus = 1;
            return false;
        }

        if (path.empty() || path == "~")
            path = home;
        else
            path = std::string(home) + path.substr(1);
    }

    std::filesystem::path newPath(path);

    if (newPath.is_relative())
        newPath = cwd / newPath;

    std::error_code ec;
    auto resolved = std::filesystem::canonical(newPath, ec);

    if (ec) {
        error = "cd: " + ec.message();
        lastStatus = 1;
        return false;
    }

    if (!std::filesystem::is_directory(resolved, ec)) {
        error = "cd: not a directory";
        lastStatus = 1;
        return false;
    }

    cwd = resolved;
    lastStatus = 0;
    return true;
}

std::string ShellSession::expandExitStatus(
    const std::string& command
) const {
    std::string result;
    std::string status = std::to_string(lastStatus);

    for (size_t i = 0; i < command.size(); i++) {
        if (command[i] == '$' &&
            i + 1 < command.size() &&
            command[i + 1] == '?') {

            result += status;
            i++;
        } else {
            result += command[i];
        }
    }

    return result;
}
