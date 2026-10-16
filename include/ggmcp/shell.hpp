#pragma once

#include <string>

namespace ggmcp {

struct ShellResult {
    bool started = false;
    bool timed_out = false;
    int exit_code = -1;
    std::string out;
    std::string err;
    std::string error;  // non-empty when the command could not be run or reaped
};

// `/bin/sh -c <command>`, with both streams captured and the whole process group
// killed on a timeout. Not confined by the root: it runs what the server user runs.
ShellResult run_shell(const std::string& command, int timeout_seconds);

}  // namespace ggmcp
