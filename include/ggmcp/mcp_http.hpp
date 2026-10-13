// ggmcp - a tiny Model Context Protocol server built on vendored single-header libraries.
//
// mcp_http.hpp - MCP "streamable HTTP" transport on top of HttpServer.
//
// Wire rules implemented here (protocol version 2025-06-18):
//   * POST   <endpoint>  JSON-RPC message in, JSON-RPC message out.
//                        Notifications get 202 Accepted with an empty body.
//   * GET    <endpoint>  405 Method Not Allowed - this server sends no
//                        server-originated messages, which the spec allows.
//   * DELETE <endpoint>  ends the session.
//   * OPTIONS<endpoint>  preflight.
//   * Mcp-Session-Id     issued on initialize, required afterwards; unknown
//                        ids get 404.
//   * MCP-Protocol-Version  validated when a client sends it.
//   * Origin             validated (DNS-rebinding protection).

#ifndef GGGMCP_MCP_HTTP_HPP
#define GGGMCP_MCP_HTTP_HPP

#include <memory>
#include <string>
#include <vector>

#include "ggmcp/auth.hpp"
#include "ggmcp/http_server.hpp"
#include "ggmcp/protocol.hpp"

namespace ggmcp {

struct HttpOptions {
    std::string endpoint = "/mcp";

    /// Bearer-token authorization, prepared with TokenStore::prepare() before the
    /// handler is built - a server that demands a token it was never given should
    /// fail at startup, not on the first request. Null (the default) means the
    /// server stays open on the wire, as it was before this option existed.
    ///
    /// The handler is shared by every connection thread, hence the shared_ptr:
    /// TokenStore owns a mutex and is neither copyable nor movable.
    std::shared_ptr<TokenStore> tokens;

    /// Origins to accept in the Origin header, e.g. "http://localhost:3000".
    /// Empty means "localhost and 127.0.0.1 on the bound port", which is what a
    /// local dev client sends. A missing Origin (non-browser clients) is always
    /// accepted.
    std::vector<std::string> allowed_origins;

    /// Accept any Origin, which removes the DNS-rebinding guard. Off here so
    /// that using this transport on its own stays careful; the `ggmcp` command
    /// line turns it *on* for a bare invocation, and --strict-origin (or
    /// naming origins with --allow-origin) turns it back off.
    bool allow_any_origin = false;

    /// Skip Origin validation entirely.
    bool disable_origin_check = false;

    /// Expose a plain GET /health status endpoint.
    bool enable_health_endpoint = true;

    /// Expose GET / with a human-readable banner.
    bool enable_index_endpoint = true;
};

/// Wraps a Dispatcher in an HTTP request handler.
HttpServer::Handler make_http_handler(Dispatcher& dispatcher, HttpOptions options, std::uint16_t bound_port);

/// Convenience overload that uses the default endpoint (/mcp).
HttpServer::Handler make_http_handler(Dispatcher& dispatcher, std::uint16_t bound_port);

/// True when `origin` is loopback and `port` matches `bound_port`.
bool origin_is_loopback(const std::string& origin, std::uint16_t bound_port);

}  // namespace ggmcp

#endif  // GGGMCP_MCP_HTTP_HPP
