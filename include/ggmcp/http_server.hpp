// ggmcp - a tiny Model Context Protocol server in dependency-free C++17.
//
// SPDX-License-Identifier: MIT
//
// http_server.hpp - just enough HTTP/1.1 to carry MCP streamable-HTTP traffic.
//
// Deliberately incomplete as an HTTP server: no TLS, no HTTP/2, no compression,
// no chunked *output* (responses always carry Content-Length). Chunked *input*
// is decoded, because clients do send it. It is not intended to face the public
// internet; bind to localhost unless you have a proxy in front.

#ifndef GGGMCP_HTTP_SERVER_HPP
#define GGGMCP_HTTP_SERVER_HPP

#include <atomic>
#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <utility>
#include <vector>

namespace ggmcp {

struct Request {
    std::string method;
    std::string target;                            ///< raw request target, e.g. /mcp?x=1
    std::string path;                              ///< target before '?'
    std::string query;                             ///< target after '?' (may be empty)
    std::string version;                           ///< "HTTP/1.1"
    std::string body;
    std::string peer;                              ///< "127.0.0.1:54321"
    std::map<std::string, std::string> headers;    ///< keys are lowercased

    /// Empty string when the header is absent.
    std::string header(const std::string& name) const;
    bool has_header(const std::string& name) const;

    bool keep_alive = true;
};

struct Response {
    int status = 200;
    std::string content_type = "application/json";
    std::string body;
    std::vector<std::pair<std::string, std::string>> headers;
    bool close = false;                            ///< force Connection: close

    void set(const std::string& name, const std::string& value);

    /// Builds the full wire form: status line, headers, blank line, body.
    std::string serialize() const;

    static Response json(int status, std::string body);
    static Response text(int status, std::string body);
    static Response empty(int status);             ///< status with no body (202/204/304)
    static Response failure(int status, const std::string& reason);
};

/// Thread-per-connection blocking HTTP server.
class HttpServer {
public:
    using Handler = std::function<Response(const Request&)>;

    HttpServer(std::string host, std::uint16_t port, Handler handler);
    ~HttpServer();

    HttpServer(const HttpServer&) = delete;
    HttpServer& operator=(const HttpServer&) = delete;

    /// Replace the handler. Needed when the handler must know the bound port
    /// (call after listen(), before serve()).
    void set_handler(Handler handler) { handler_ = std::move(handler); }

    /// Binds and listens. Returns false and fills `error` on failure.
    bool listen(std::string& error);

    /// Accepts connections until stop() is called or the process is signalled.
    int serve();

    /// Asks serve() and all connection loops to unwind. Safe from a signal handler.
    void stop() { stop_.store(true); }
    bool stopping() const { return stop_.load(); }

    /// Port actually bound (differs from the requested one when it was 0).
    std::uint16_t port() const { return bound_port_; }

    HttpServer& max_connections(std::size_t n)   { max_connections_ = n; return *this; }
    HttpServer& idle_timeout_seconds(int secs)   { idle_timeout_ = secs; return *this; }
    HttpServer& max_body_bytes(std::size_t n)    { max_body_bytes_ = n; return *this; }

    std::size_t active_connections() const { return active_.load(); }

private:
    void serve_connection(int fd, std::string peer);

    std::string host_;
    std::uint16_t requested_port_;
    std::uint16_t bound_port_ = 0;
    Handler handler_;
    int listen_fd_ = -1;

    std::atomic<bool> stop_{false};
    std::atomic<std::size_t> active_{0};

    std::size_t max_connections_ = 64;
    int idle_timeout_ = 75;
    std::size_t max_body_bytes_ = 1u << 20;
};

/// Maps SIGINT/SIGTERM to a flag so serve() returns instead of dying in a
/// handler, which would skip connection teardown.
void install_signal_handlers();
bool stop_requested();

}  // namespace ggmcp

#endif  // GGGMCP_HTTP_SERVER_HPP
