// The two transports. Both drive the same gg::mcp::Protocol.

#pragma once

#include <memory>
#include <string>

#include "gg/http.hpp"
#include "gg/mcp/auth.hpp"
#include "gg/mcp/config.hpp"
#include "gg/mcp/protocol.hpp"
#include "gg/mcp/session.hpp"
#include "gg/mcp/tools.hpp"

namespace gg::mcp {

// Streamable HTTP transport: POST/DELETE on the endpoint, plus /health, the
// index page and the RFC 9728 resource metadata document.
class HttpServer {
public:
    explicit HttpServer(Options options);
    ~HttpServer();

    // Binds the socket. Returns false with a message when that fails.
    bool start(std::string& error);

    // Blocks until stop() is called from another thread.
    void wait();

    void stop();

    int port() const;
    std::string describe() const;

    // Exposed for tests: the router the server serves.
    std::shared_ptr<http::Router> router() const { return router_; }

private:
    void buildRouter();

    Options options_;
    ToolRegistry tools_;
    SessionStore sessions_;
    Authorizer authorizer_;
    std::unique_ptr<Protocol> protocol_;
    std::shared_ptr<http::Router> router_;
    std::unique_ptr<http::IServer> server_;
};

// stdio transport: one JSON-RPC message per line on stdin, replies on stdout.
bool runStdio(const Options& options, std::string& error);

// Shared wiring, also used by the end to end tests.
std::shared_ptr<http::Router> makeRouter(const Options& options, Protocol& protocol,
                                         const Authorizer& authorizer, SessionStore& sessions,
                                         const ToolRegistry& tools);

void logLine(const Options& options, const std::string& line);

} // namespace gg::mcp
