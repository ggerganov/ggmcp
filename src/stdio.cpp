#include "ggmcp/stdio.hpp"

#include <poll.h>
#include <string.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <string>

#include "ggmcp/config.hpp"
#include "ggmcp/json.hpp"
#include "ggmcp/protocol.hpp"

namespace ggmcp {
namespace {

// One response per line, flushed on its own: a client reads the stream line by
// line and must see each answer as soon as it is written.
bool write_line(int fd, const std::string& text) {
    std::string body = text;
    body += '\n';
    std::size_t left = body.size();
    const char* at = body.data();
    while (left > 0) {
        const ssize_t written = ::write(fd, at, left);
        if (written > 0) {
            at += written;
            left -= static_cast<std::size_t>(written);
            continue;
        }
        if (written < 0 && errno == EINTR) continue;
        return false;
    }
    return true;
}

}  // namespace

int run_stdio(const Config& config, Core& core) {
    (void)config;

    std::string pending;  // bytes read but not yet closed by a newline
    char chunk[8192];

    for (;;) {
        struct pollfd watched[2];
        nfds_t count = 0;
        watched[count++] = {STDIN_FILENO, POLLIN, 0};
        const int wakeup = core.wakeup();
        if (wakeup >= 0) watched[count++] = {wakeup, POLLIN, 0};

        const int ready = ::poll(watched, count, -1);
        if (core.stopping()) return 0;
        if (ready < 0) {
            if (errno == EINTR) continue;
            return 0;
        }

        if ((watched[0].revents & POLLIN) == 0) {
            if ((watched[0].revents & (POLLHUP | POLLERR | POLLNVAL)) != 0) return 0;
            continue;
        }

        const ssize_t got = ::read(STDIN_FILENO, chunk, sizeof(chunk));
        if (got <= 0) return 0;  // stdin closed, or the terminal went away
        pending.append(chunk, static_cast<std::size_t>(got));

        std::size_t edge = 0;
        while ((edge = pending.find('\n')) != std::string::npos) {
            const std::string line = pending.substr(0, edge);
            pending.erase(0, edge + 1);
            if (line.find_first_not_of(" \t\r\n") == std::string::npos) continue;

            RequestEnvelope env;
            env.transport = "stdio";
            env.address = "stdio";

            const Outcome outcome = core.handle_body(line, env);
            if (!outcome.has_body) continue;  // a notification is answered with nothing
            if (!write_line(STDOUT_FILENO, dump_safely(outcome.body))) return 0;
        }
    }
}

}  // namespace ggmcp
