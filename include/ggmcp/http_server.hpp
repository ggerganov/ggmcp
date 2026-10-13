// ggmcp - a tiny Model Context Protocol server built on vendored single-header libraries.
//
// SPDX-License-Identifier: MIT
//
// http_server.hpp - the HTTP surface this project serves.
//
// The protocol work - reading requests off a socket, parsing the request line
// and headers, decoding Content-Length and chunked bodies, framing responses,
// keep-alive, worker threads - is cpp-httplib's (vendor/httplib.h). None of it
// lives here; what remains is the vocabulary the rest of ggmcp speaks: a
// Request, a Response, and an HttpServer that passes every request to a
// handler and writes back what the handler returns.
//
// Still deliberately not an internet-facing server: cpp-httplib is used
// without TLS, so bind to localhost unless you have a proxy in front.

#ifndef GGGMCP_HTTP_SERVER_HPP
#define GGGMCP_HTTP_SERVER_HPP

#include <atomic>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace ggmcp {

struct Request {
    std::string method;                            ///< lowercased, e.g. "post"
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
};

struct Response {
    int status = 200;
    std::string content_type = "application/json";
    std::string body;
    std::vector<std::pair<std::string, std::string>> headers;

    /// Replaces any header of the same name (case-insensitively).
    void set(const std::string& name, const std::string& value);

    static Response json(int status, std::string body);
    static Response text(int status, std::string body);
    static Response empty(int status);             ///< status with no body (202/204/304)
};

/// A thin host for cpp-httplib: bind, serve, stop.
///
/// Connection reuse, request framing and the worker pool are the library's
/// concern; this class only owns the endpoint configuration and the bridge
/// between `Handler` and `httplib::Server`.
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

    /// Serves until stop() is called or the process is signalled.
    int serve();

    /// Asks serve() to unwind. Safe from a signal handler: it only raises a
    /// flag, which a side thread turns into cpp-httplib's Server::stop().
    void stop() { stop_.store(true); }
    bool stopping() const { return stop_.load(); }

    /// Port actually bound (differs from the requested one when it was 0).
    std::uint16_t port() const { return bound_port_; }

    /// How many requests may be served at once - cpp-httplib sizes its worker
    /// pool from this instead of spawning a thread per connection.
    HttpServer& max_connections(std::size_t n)   { max_connections_ = n; return *this; }
    /// Seconds a client may take to send a request, and to idle on a kept-alive
    /// connection, before it is dropped.
    HttpServer& idle_timeout_seconds(int secs)   { idle_timeout_ = secs; return *this; }
    /// Largest request body accepted; larger ones are refused with 413.
    HttpServer& max_body_bytes(std::size_t n)    { max_body_bytes_ = n; return *this; }

    /// Requests currently inside the handler (see max_connections()).
    std::size_t active_connections() const { return active_.load(); }

private:
    struct Impl;                                 ///< owns the cpp-httplib server

    std::string host_;
    std::uint16_t requested_port_;
    std::uint16_t bound_port_ = 0;
    Handler handler_;
    std::unique_ptr<Impl> impl_;

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
