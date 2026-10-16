#include "ggmcp/shell.hpp"

#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <cstring>

#include <fcntl.h>
#include <poll.h>
#include <sys/wait.h>
#include <unistd.h>

namespace ggmcp {
namespace {

void set_nonblocking(int fd) {
    const int flags = ::fcntl(fd, F_GETFL, 0);
    if (flags >= 0) ::fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

// Empty the pipe. True means the far end is gone and nothing more will arrive.
bool drain(int fd, std::string& into) {
    for (;;) {
        char buffer[65536];
        const ssize_t n = ::read(fd, buffer, sizeof(buffer));
        if (n > 0) {
            into.append(buffer, static_cast<std::size_t>(n));
            continue;
        }
        if (n == 0) return true;
        if (n < 0 && errno == EINTR) continue;
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) return false;
        return true;
    }
}

void wait_a_bit(int fd, std::string& into, int wait_ms) {
    struct pollfd pfd;
    pfd.fd = fd;
    pfd.events = POLLIN;
    pfd.revents = 0;
    if (::poll(&pfd, 1, wait_ms) > 0) drain(fd, into);
}

}  // namespace

ShellResult run_shell(const std::string& command, int timeout_seconds) {
    ShellResult result;

    int out_pipe[2] = {-1, -1};
    int err_pipe[2] = {-1, -1};
    if (::pipe(out_pipe) != 0) {
        result.error = std::string("pipe: ") + std::strerror(errno);
        return result;
    }
    if (::pipe(err_pipe) != 0) {
        result.error = std::string("pipe: ") + std::strerror(errno);
        ::close(out_pipe[0]);
        ::close(out_pipe[1]);
        return result;
    }

    const pid_t pid = ::fork();
    if (pid < 0) {
        result.error = std::string("fork: ") + std::strerror(errno);
        ::close(out_pipe[0]);
        ::close(out_pipe[1]);
        ::close(err_pipe[0]);
        ::close(err_pipe[1]);
        return result;
    }

    if (pid == 0) {
        ::setpgid(0, 0);
        ::dup2(out_pipe[1], STDOUT_FILENO);
        ::dup2(err_pipe[1], STDERR_FILENO);
        ::close(out_pipe[0]);
        ::close(out_pipe[1]);
        ::close(err_pipe[0]);
        ::close(err_pipe[1]);
        const int null = ::open("/dev/null", O_RDONLY);
        if (null >= 0) {
            ::dup2(null, STDIN_FILENO);
            ::close(null);
        }
        // An ignored signal survives exec; the child is meant to die like any other.
        ::signal(SIGPIPE, SIG_DFL);
        ::signal(SIGINT, SIG_DFL);
        ::signal(SIGTERM, SIG_DFL);
        ::signal(SIGQUIT, SIG_DFL);
        ::signal(SIGTSTP, SIG_DFL);
        ::execl("/bin/sh", "sh", "-c", command.c_str(), static_cast<char*>(nullptr));
        ::_exit(127);
    }

    ::setpgid(pid, 0);
    ::close(out_pipe[1]);
    ::close(err_pipe[1]);
    set_nonblocking(out_pipe[0]);
    set_nonblocking(err_pipe[0]);
    result.started = true;

    const int budget = timeout_seconds < 1 ? 1 : timeout_seconds;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(budget);

    bool out_open = true;
    bool err_open = true;
    bool timed_out = false;
    while (out_open || err_open) {
        const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(
            deadline - std::chrono::steady_clock::now());
        if (left.count() <= 0) {
            timed_out = true;
            break;
        }
        int wait_ms = static_cast<int>(left.count() > 200 ? 200 : left.count());
        if (wait_ms < 1) wait_ms = 1;

        struct pollfd fds[2];
        nfds_t count = 0;
        if (out_open) fds[count++] = {out_pipe[0], POLLIN, 0};
        if (err_open) fds[count++] = {err_pipe[0], POLLIN, 0};

        const int ready = ::poll(fds, count, wait_ms);
        if (ready < 0) {
            if (errno == EINTR) continue;
            break;
        }

        for (nfds_t i = 0; i < count; ++i) {
            if (fds[i].revents == 0) continue;
            const bool is_out = fds[i].fd == out_pipe[0];
            const bool closed =
                drain(is_out ? out_pipe[0] : err_pipe[0], is_out ? result.out : result.err);
            if (!closed) continue;
            if (is_out) {
                out_open = false;
            } else {
                err_open = false;
            }
        }
    }

    if (timed_out) {
        result.timed_out = true;
        ::kill(-pid, SIGKILL);
        if (out_open) wait_a_bit(out_pipe[0], result.out, 200);
        if (err_open) wait_a_bit(err_pipe[0], result.err, 200);
    }

    ::close(out_pipe[0]);
    ::close(err_pipe[0]);

    int status = 0;
    bool reaped = false;
    for (int spin = 0; spin < 200; ++spin) {
        const pid_t gone = ::waitpid(pid, &status, spin > 0 ? WNOHANG : 0);
        if (gone == pid) {
            reaped = true;
            break;
        }
        if (gone < 0 && errno != EINTR) break;
        if (gone == 0) ::usleep(10 * 1000);
    }

    if (!reaped) {
        result.error = "the command could not be reaped";
        return result;
    }

    if (WIFEXITED(status)) {
        result.exit_code = WEXITSTATUS(status);
    } else if (WIFSIGNALED(status)) {
        result.exit_code = 128 + WTERMSIG(status);
    }
    return result;
}

}  // namespace ggmcp
