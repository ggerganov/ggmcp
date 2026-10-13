// ggmcp - a tiny Model Context Protocol server built on vendored single-header libraries.
//
// SPDX-License-Identifier: MIT

#include "ggmcp/log.hpp"

#include <atomic>
#include <cstdio>

namespace ggmcp {
namespace {
std::atomic<bool> g_verbose{false};
}  // namespace

void set_verbose(bool on) { g_verbose.store(on, std::memory_order_relaxed); }
bool verbose()            { return g_verbose.load(std::memory_order_relaxed); }

void log_line(std::string_view what) {
    // stdout is the protocol channel. Diagnostics go to stderr, always.
    std::fprintf(stderr, "[ggmcp] %.*s\n", static_cast<int>(what.size()), what.data());
    std::fflush(stderr);
}

void log_debug(std::string_view what) {
    if (verbose()) log_line(what);
}

}  // namespace ggmcp
