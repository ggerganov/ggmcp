// ggmcp - a tiny Model Context Protocol server in dependency-free C++17.
//
// SPDX-License-Identifier: MIT
//
// tools.hpp - the demo tools shipped with the ggmcp binary.

#ifndef GGGMCP_TOOLS_HPP
#define GGGMCP_TOOLS_HPP

#include "ggmcp/protocol.hpp"

namespace ggmcp {

inline constexpr const char* kDefaultServerName    = "ggmcp";
inline constexpr const char* kDefaultServerVersion = "0.1.0";

/// Registers hello / echo / utc_now / whoami on `dispatcher`.
void register_demo_tools(Dispatcher& dispatcher);

}  // namespace ggmcp

#endif  // GGGMCP_TOOLS_HPP
