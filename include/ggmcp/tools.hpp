// ggmcp - a tiny Model Context Protocol server built on vendored single-header libraries.
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

/// Shared MCP tool annotations block (title + the four hint flags).
Value tool_annotations(const std::string& title, bool read_only, bool destructive, bool idempotent,
                       bool open_world = false);

/// Settings for the file and shell tools. These are deliberately OFF unless an
/// operator turns them on: a `write`/`edit`/`bash` tool on an unauthenticated
/// HTTP port is remote code execution with the server's privileges.
struct FileSystemOptions {
    std::string root = ".";            ///< confinement root for read/write/edit
    bool enable_file_tools = false;    ///< read / write / edit
    bool enable_bash = false;          ///< bash - a shell, so `root` cannot contain it
    bool allow_outside_root = false;   ///< drop confinement (only sane with a real sandbox)
    int bash_timeout_seconds = 30;     ///< wall-clock budget per command
    std::size_t max_read_bytes = 256 * 1024;
    std::size_t max_write_bytes = 8 * 1024 * 1024;

    std::string root_description() const;   ///< for tool descriptions / logs
};

/// Registers read / write / edit (and bash, when allowed) on `dispatcher`.
void register_filesystem_tools(Dispatcher& dispatcher, const FileSystemOptions& options);

/// Canonicalises `path` against `root` and checks it does not escape.
///
/// Symlinks are resolved *before* the containment test, which is the whole
/// point: a lexical check on the raw string is what lets `../../etc/passwd`
/// or a symlinked-out-of-tree link through. Returns false with a model-facing
/// message when the path is unusable.
bool confine_path(const std::string& root, const std::string& path, bool allow_outside,
                  std::string& resolved, std::string& error);

}  // namespace ggmcp

#endif  // GGGMCP_TOOLS_HPP
