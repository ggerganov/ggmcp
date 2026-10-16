#include <signal.h>
#include <sys/stat.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <string>
#include <system_error>
#include <thread>

#include "ggmcp/auth.hpp"
#include "ggmcp/config.hpp"
#include "ggmcp/http.hpp"
#include "ggmcp/json.hpp"
#include "ggmcp/log.hpp"
#include "ggmcp/paths.hpp"
#include "ggmcp/protocol.hpp"
#include "ggmcp/session.hpp"
#include "ggmcp/stdio.hpp"
#include "ggmcp/tools.hpp"
#include "ggmcp/version.hpp"

namespace {

std::atomic<bool> g_stop{false};
std::atomic<ggmcp::Core*> g_core{nullptr};

// A signal handler may only do async-signal-safe work: set a flag, and poke the
// core's self-pipe so a loop blocked on a read wakes up.
void on_signal(int) {
    g_stop.store(true);
    ggmcp::Core* core = g_core.load();
    if (core != nullptr) core->stop();
}

void install_signals() {
    struct sigaction action {};
    sigemptyset(&action.sa_mask);
    action.sa_handler = on_signal;
    action.sa_flags = SA_RESTART;
    sigaction(SIGINT, &action, nullptr);
    sigaction(SIGTERM, &action, nullptr);
    ::signal(SIGPIPE, SIG_IGN);
    ::signal(SIGTSTP, SIG_IGN);
    ::signal(SIGTTOU, SIG_IGN);
    ::signal(SIGTTIN, SIG_IGN);
}

std::string joined(const std::vector<std::string>& items) {
    std::string out;
    for (std::size_t i = 0; i < items.size(); ++i) {
        if (i != 0) out += ", ";
        out += items[i];
    }
    return out;
}

void banner(const ggmcp::Config& config, const ggmcp::ToolRegistry& tools,
            const ggmcp::TokenStore& auth, const ggmcp::Log& log) {
    log.line(std::string("ggmcp ") + ggmcp::kVersion);
    log.line("  transport  : " + std::string(to_string(config.transport)));
    if (config.transport == ggmcp::Transport::Http) {
        log.line("  endpoint   : " + config.url());
        log.line("  index      : http://" + config.host + ":" + std::to_string(config.port) + "/");
        log.line("  origins    : " + std::string(config.cors ? (config.strict_origin ? "strict" : "any")
                                                              : "cors off"));
    }
    log.line("  json       : " + std::string(ggmcp::kJsonBackend));
    log.line("  http       : " + std::string(ggmcp::kHttpBackend));
    log.line("  policy     : " + std::string(to_string(config.session_policy)));
    log.line("  root       : " + config.root_description());
    log.line("  auth       : " + auth.source());
    log.line("  alias      : " + (config.alias.empty() ? std::string("(none)") : config.alias));
    log.line("  tools      : " + joined(tools.names()));
}

}  // namespace

int main(int argc, char** argv) {
    ggmcp::Config config;
    std::string error;

    switch (ggmcp::parse_args(argc, argv, config, error)) {
        case ggmcp::ParseResult::Help:
            std::fputs(ggmcp::help_text().c_str(), stdout);
            std::fflush(stdout);
            return 0;
        case ggmcp::ParseResult::Version:
            std::printf("ggmcp %s\n", ggmcp::kVersion);
            std::fflush(stdout);
            return 0;
        case ggmcp::ParseResult::Error:
            std::fprintf(stderr, "ggmcp: %s\n", error.c_str());
            return 2;
        case ggmcp::ParseResult::Ok:
            break;
    }

    const bool stdio = config.transport == ggmcp::Transport::Stdio;
    const ggmcp::Log log(config.quiet, stdio);

    if (config.fs_root.empty()) {
        std::fprintf(stderr, "ggmcp: no home directory to serve; pass --fs-root\n");
        return 2;
    }
    std::error_code ec;
    if (!std::filesystem::is_directory(config.fs_root, ec)) {
        std::fprintf(stderr, "ggmcp: the root \"%s\" is not a directory\n", config.fs_root.c_str());
        return 2;
    }

    ggmcp::TokenStore auth(config);
    if (!auth.start(error)) {
        std::fprintf(stderr, "ggmcp: %s\n", error.c_str());
        return 2;
    }

    ggmcp::SessionStore sessions;
    const ggmcp::ToolRegistry tools(config);
    ggmcp::Core core(config, sessions, tools);
    g_core.store(&core);
    install_signals();

    if (stdio) {
        banner(config, tools, auth, log);
        return ggmcp::run_stdio(config, core);
    }

    ggmcp::HttpTransport transport(config, core, auth, tools);
    if (!transport.bind(error)) {
        std::fprintf(stderr, "ggmcp: %s\n", error.c_str());
        return 2;
    }

    banner(config, tools, auth, log);

    std::thread serving([&transport]() { transport.run(); });
    while (!g_stop.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(25));
    }
    transport.stop();
    serving.join();
    return 0;
}
