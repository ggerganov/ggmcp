// ggmcp - a tiny Model Context Protocol server built on vendored single-header libraries.
//
// SPDX-License-Identifier: MIT
//
// log.hpp - stderr logging. stdout is the stdio protocol channel and must
// never receive anything but JSON-RPC frames, so all diagnostics go elsewhere.

#ifndef GGGMCP_LOG_HPP
#define GGGMCP_LOG_HPP

#include <string_view>

namespace ggmcp {

/// Off by default. Enable with --verbose / -v.
void set_verbose(bool on);
bool verbose();

/// Always printed.
void log_line(std::string_view what);

/// Printed only when verbose.
void log_debug(std::string_view what);

}  // namespace ggmcp

#endif  // GGGMCP_LOG_HPP
