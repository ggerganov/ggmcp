// The bash tool: /bin/sh -c with a wall clock timeout. POSIX only.

#include "gg/mcp/tools.hpp"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#if !defined(_WIN32)
#include <csignal>
#include <fcntl.h>
#include <poll.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace gg::mcp {
namespace {

constexpr std::size_t kMaxOutput = 1u << 20;

Json describe(const ShellResult& result, const std::string& command) {
    return Json::object({{"command", command},
                         {"exitCode", static_cast<long long>(result.exitCode)},
                         {"timedOut", result.timedOut},
                         {"durationMs", static_cast<long long>(result.durationMs)},
                         {"output", result.output},
                         {"error", result.errorText}});
}

#if !defined(_WIN32)

void drain(int fd, std::string& into, bool& closed, bool& truncated) {
    for (;;) {
        char buffer[4096];
        const ssize_t read = ::read(fd, buffer, sizeof(buffer));
        if (read == 0) {
            closed = true;
            return;
        }
        if (read < 0) {
            if (errno == EINTR) {
                continue;
            }
            closed = errno != EAGAIN && errno != EWOULDBLOCK;
            return;
        }
        const std::size_t room =
            into.size() < kMaxOutput ? kMaxOutput - into.size() : 0;
        into.append(buffer, std::min(room, static_cast<std::size_t>(read)));
        if (static_cast<std::size_t>(read) > room) {
            truncated = true;
        }
    }
}

void makeNonBlocking(int fd) {
    const int flags = ::fcntl(fd, F_GETFL, 0);
    if (flags >= 0) {
        ::fcntl(fd, F_SETFL, flags | O_NONBLOCK);
    }
}

} // namespace

ShellResult runShell(const std::string& command, int timeoutSeconds) {
    ShellResult result;
    const auto started = std::chrono::steady_clock::now();
    const auto deadline = started + std::chrono::seconds(timeoutSeconds);

    int outPipe[2] = {-1, -1};
    int errPipe[2] = {-1, -1};
    if (::pipe(outPipe) != 0) {
        result.errorText = std::strerror(errno);
        return result;
    }
    if (::pipe(errPipe) != 0) {
        const int code = errno;
        ::close(outPipe[0]);
        ::close(outPipe[1]);
        result.errorText = std::strerror(code);
        return result;
    }
    makeNonBlocking(outPipe[0]);
    makeNonBlocking(errPipe[0]);

    const pid_t pid = ::fork();
    if (pid < 0) {
        const int code = errno;
        ::close(outPipe[0]);
        ::close(outPipe[1]);
        ::close(errPipe[0]);
        ::close(errPipe[1]);
        result.errorText = std::strerror(code);
        return result;
    }

    if (pid == 0) {
        ::setsid();
        ::dup2(outPipe[1], STDOUT_FILENO);
        ::dup2(errPipe[1], STDERR_FILENO);
        ::close(outPipe[0]);
        ::close(outPipe[1]);
        ::close(errPipe[0]);
        ::close(errPipe[1]);
        const int devNull = ::open("/dev/null", O_RDONLY);
        if (devNull >= 0) {
            ::dup2(devNull, STDIN_FILENO);
            if (devNull > STDERR_FILENO) {
                ::close(devNull);
            }
        } else {
            ::close(STDIN_FILENO);
        }
        ::execl("/bin/sh", "sh", "-c", command.c_str(), static_cast<char*>(nullptr));
        ::_exit(127);
    }

    ::close(outPipe[1]);
    ::close(errPipe[1]);

    bool outClosed = false;
    bool errClosed = false;
    bool truncated = false;
    bool reaped = false;
    int status = 0;

    for (;;) {
        if (outPipe[0] >= 0 && !outClosed) {
            drain(outPipe[0], result.output, outClosed, truncated);
        }
        if (errPipe[0] >= 0 && !errClosed) {
            drain(errPipe[0], result.errorText, errClosed, truncated);
        }
        if (outClosed && errClosed) {
            break;
        }

        const auto now = std::chrono::steady_clock::now();
        if (now >= deadline) {
            result.timedOut = true;
            break;
        }

        const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
            deadline - now).count();
        std::vector<pollfd> descriptors;
        if (!outClosed) {
            descriptors.push_back(pollfd {outPipe[0], POLLIN, 0});
        }
        if (!errClosed) {
            descriptors.push_back(pollfd {errPipe[0], POLLIN, 0});
        }
        if (descriptors.empty()) {
            break;
        }
        const int ready = ::poll(descriptors.data(), descriptors.size(),
                                 static_cast<int>(std::min<long long>(remaining, 50)));
        if (ready < 0 && errno != EINTR) {
            break;
        }

        if (!reaped) {
            const pid_t done = ::waitpid(pid, &status, WNOHANG);
            if (done == pid) {
                reaped = true;
                break;
            }
        }
    }

    if (result.timedOut) {
        ::kill(-pid, SIGKILL);
    }
    if (!reaped) {
        while (::waitpid(pid, &status, 0) < 0 && errno == EINTR) {
            continue;
        }
    }

    drain(outPipe[0], result.output, outClosed, truncated);
    drain(errPipe[0], result.errorText, errClosed, truncated);
    ::close(outPipe[0]);
    ::close(errPipe[0]);

    if (truncated) {
        result.output += "\n... output truncated ...";
    }

    if (result.timedOut) {
        result.exitCode = 124;
        result.errorText += "timed out after " + std::to_string(timeoutSeconds) + "s\n";
    } else if (WIFEXITED(status)) {
        result.exitCode = WEXITSTATUS(status);
    } else if (WIFSIGNALED(status)) {
        result.exitCode = 128 + WTERMSIG(status);
    }

    result.durationMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                            std::chrono::steady_clock::now() - started)
                            .count();
    return result;
}

#else

ShellResult runShell(const std::string&, int) {
    ShellResult result;
    result.errorText = "the bash tool needs a POSIX host";
    return result;
}

} // namespace

#endif

ToolResult runShellTool(const Json& arguments, int timeoutSeconds) {
    if (!arguments.hasText("command")) {
        return ToolResult::error(
            Json::object({{"error", "bash needs a string 'command'"}}).dump());
    }
    long long requested = arguments.getInt("timeout", timeoutSeconds);
    if (requested < 1) {
        requested = 1;
    }
    if (requested > 3600) {
        requested = 3600;
    }

    const std::string command = arguments.at("command").asString();
    const ShellResult result = runShell(command, static_cast<int>(requested));
    const Json body = describe(result, command);
    if (result.timedOut || result.exitCode < 0) {
        return ToolResult::error(body.dump());
    }
    return ToolResult::ok(body.dump());
}

} // namespace gg::mcp
