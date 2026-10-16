#include "gg/mcp/server.hpp"

#include <cstdio>
#include <iostream>
#include <string>

namespace gg::mcp {

bool runStdio(const Options& options, std::string& error) {
    Options settings = options;
    // The stdio transport has no session handling: one message per line, no
    // session ids, so every session policy behaves like the stateless one.
    settings.sessionPolicy = SessionPolicy::Stateless;
    settings.requireToken = false;

    ToolRegistry tools;
    registerBuiltins(tools, settings);
    tools.setAlias(settings.alias);
    SessionStore sessions;
    Authorizer authorizer(settings);
    Protocol protocol(settings, tools, sessions);

    logLine(settings, "ggmcp " + settings.serverVersion + " over stdio, " +
                          std::to_string(tools.size()) + " tools");

    std::string line;
    while (std::getline(std::cin, line)) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        if (line.empty()) {
            continue;
        }
        const std::string reply = protocol.handleLine(line);
        if (reply.empty()) {
            continue;
        }
        if (!(std::cout << reply << "\n") || !std::cout.flush()) {
            error = "cannot write the reply to stdout";
            return false;
        }
    }
    return true;
}

} // namespace gg::mcp
