#include "gg/backends.hpp"
#include "gg/mcp/config.hpp"
#include "gg/mcp/server.hpp"

#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>

#include <csignal>
#include <pthread.h>
#include <unistd.h>

namespace {

int fail(const std::string& message, const std::string& usage) {
    std::fprintf(stderr, "ggmcp: %s\n", message.c_str());
    if (!usage.empty()) {
        std::fprintf(stderr, "%s\n", usage.c_str());
    }
    std::fflush(stderr);
    return 2;
}

} // namespace

int main(int argc, char** argv) {
    using gg::mcp::HttpServer;
    using gg::mcp::Options;
    using gg::mcp::Transport;

    Options options;
    std::string error;
    std::string usage;
    if (!gg::mcp::parseOptions(argc, argv, options, error, usage)) {
        return fail(error, usage);
    }
    if (!usage.empty()) {
        std::printf("%s\n", usage.c_str());
        std::fflush(stdout);
        return 0;
    }

    if (!gg::backends::selectJsonBackend(options.jsonBackend, error)) {
        return fail(error, "");
    }
    if (options.transport == Transport::Http && !gg::backends::selectHttpBackend(
                                                    options.httpBackend, error)) {
        return fail(error, "");
    }

    if (options.root().empty()) {
        return fail("the file tools need a root: pass --fs-root or set $HOME", "");
    }

    if (options.transport == Transport::Stdio) {
        std::string problem;
        if (!gg::mcp::runStdio(options, problem)) {
            return fail(problem, "");
        }
        return 0;
    }

    HttpServer server(options);
    if (!server.start(error)) {
        return fail(error, "");
    }

    if (!options.quiet) {
        std::fprintf(stderr, "%s\n", server.describe().c_str());
        std::fflush(stderr);
    }

    ::signal(SIGPIPE, SIG_IGN);

    sigset_t blocked;
    sigemptyset(&blocked);
    sigaddset(&blocked, SIGINT);
    sigaddset(&blocked, SIGTERM);
    pthread_sigmask(SIG_BLOCK, &blocked, nullptr);

    std::thread waiter([&server, &blocked, &options] {
        int signal_ = 0;
        if (sigwait(&blocked, &signal_) == 0) {
            gg::mcp::logLine(options, "signal " + std::to_string(signal_) + ", shutting down");
        }
        server.stop();
    });

    server.wait();
    server.stop();
    waiter.join();

    gg::mcp::logLine(options, "bye");
    return 0;
}
