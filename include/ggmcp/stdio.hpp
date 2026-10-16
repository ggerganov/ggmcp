#pragma once

namespace ggmcp {

struct Config;
class Core;

// The stdio transport: one JSON-RPC message per line on stdin, one JSON-RPC
// response per line on stdout. Reads block, notifications are answered with
// nothing, and EOF on stdin closes the process with 0. A signal that sets the
// core's stop flag ends it too, even with the terminal still open.
int run_stdio(const Config& config, Core& core);

}  // namespace ggmcp
