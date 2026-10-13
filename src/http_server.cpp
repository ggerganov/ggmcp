// ggmcp - a tiny Model Context Protocol server built on vendored single-header libraries.
//
// SPDX-License-Identifier: MIT
//
// http_server.cpp - cpp-httplib (vendor/httplib.h) behind ggmcp's HttpServer.
//
// Everything that used to live here is gone: no socket(), no poll(), no
// request-line or header parsing, no chunked decoder, no response serializer,
// no thread-per-connection loop. cpp-httplib does all of that; this file is
// the bridge - it binds, registers one handler for every request, converts
// between the library's Request/Response and this project's, and turns the
// signal flag into the library's stop().

#include "ggmcp/http_server.hpp"

#include "ggmcp/log.hpp"

#include <atomic>
#include <cctype>
#include <chrono>
#include <csignal>
#include <string>
#include <thread>

#include <sys/socket.h>

#include <httplib.h>

namespace ggmcp {
namespace {

// sig_atomic_t lives in the global namespace on every libc we target; std::
// sig_atomic_t is optional in the standard, so do not rely on it.
volatile sig_atomic_t g_stop_requested = 0;

void on_signal(int) { g_stop_requested = 1; }

std::string lower(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

/// cpp-httplib keeps headers in a case-insensitive multi-map; the rest of
/// ggmcp reads them out of a lowercase, one-value-per-name map, which is what
/// a plain HTTP/1.1 parser hands over. Last value wins, as before.
///
/// The REMOTE_*/LOCAL_* names are the library's own bookkeeping rather than
/// bytes the client sent, so they are dropped: nothing in ggmcp reads them
/// (the peer comes from the socket), and skipping them keeps the map free of
/// names no well-behaved client could have produced.
void copy_headers(const httplib::Request& raw, Request& request) {
    for (const auto& header : raw.headers) {
        const std::string key = lower(header.first);
        if (key == "remote_addr" || key == "remote_port" || key == "local_addr" || key == "local_port") continue;
        request.headers[key] = header.second;
    }
}

/// One httplib request in, one ggmcp request out.
Request adapt_request(const httplib::Request& raw) {
    Request request;
    request.method = lower(raw.method);
    request.target = raw.target;
    request.path = raw.path;
    const std::size_t separator = request.target.find('?');
    request.query = separator == std::string::npos ? std::string() : request.target.substr(separator + 1);
    request.version = raw.version;
    request.body = raw.body;
    request.peer = raw.remote_addr.empty()
                       ? std::string("-")
                       : raw.remote_addr +
                             (raw.remote_port > 0 ? ":" + std::to_string(raw.remote_port) : std::string());
    copy_headers(raw, request);
    return request;
}

/// Response headers are set, not accumulated: cpp-httplib's set_header()
/// appends, which would emit a header twice if a handler replaced one.
void set_header(httplib::Response& out, const std::string& name, const std::string& value) {
    const auto range = out.headers.equal_range(name);
    out.headers.erase(range.first, range.second);
    out.set_header(name, value);
}

/// One ggmcp response out, one httplib response written back.
void adapt_response(const Response& response, httplib::Response& out) {
    out.status = response.status;
    if (response.content_type.empty()) {
        out.body = response.body;   // a bare status: 202 Accepted, 204 No Content
    } else {
        out.set_content(response.body, response.content_type);
    }
    for (const auto& header : response.headers) set_header(out, header.first, header.second);
}

/// The one place a request enters ggmcp and the one place an answer leaves.
///
/// Also the place that stops a throwing handler from taking the connection -
/// and with it the worker thread - down with it: cpp-httplib turns an
/// unhandled exception into a 500 without a body, which is a worse answer
/// than saying what went wrong.
void dispatch(const HttpServer::Handler& handler, std::atomic<std::size_t>& active,
              const httplib::Request& raw, httplib::Response& out) {
    const Request request = adapt_request(raw);

    Response response;
    ++active;
    try {
        response = handler(request);
    } catch (const std::exception& e) {
        log_line("handler threw for " + request.method + " " + request.path + ": " + e.what());
        response = Response::text(500, "Internal Server Error: the handler failed\n");
    }
    --active;

    adapt_response(response, out);
    log_debug(request.method + " " + request.path + " -> " + std::to_string(response.status));
}

}  // namespace

std::string Request::header(const std::string& name) const {
    const auto it = headers.find(lower(name));
    return it == headers.end() ? std::string() : it->second;
}

bool Request::has_header(const std::string& name) const { return headers.count(lower(name)) != 0; }

void Response::set(const std::string& name, const std::string& value) {
    for (auto& kv : headers) {
        if (lower(kv.first) == lower(name)) {
            kv.second = value;
            return;
        }
    }
    headers.emplace_back(name, value);
}

Response Response::json(int status, std::string body) {
    Response r;
    r.status = status;
    r.content_type = "application/json";
    r.body = std::move(body);
    return r;
}

Response Response::text(int status, std::string body) {
    Response r;
    r.status = status;
    r.content_type = "text/plain; charset=utf-8";
    r.body = std::move(body);
    return r;
}

Response Response::empty(int status) {
    Response r;
    r.status = status;
    r.content_type.clear();
    r.body.clear();
    return r;
}

// ---------------------------------------------------------------------------
// HttpServer
// ---------------------------------------------------------------------------

/// The library lives behind the pimpl so that http_server.hpp - the header
/// the rest of ggmcp, and its tests, include - stays free of a 340 KB include.
struct HttpServer::Impl {
    httplib::Server server;
};

HttpServer::HttpServer(std::string host, std::uint16_t port, Handler handler)
    : host_(std::move(host)),
      requested_port_(port),
      handler_(std::move(handler)),
      impl_(std::make_unique<Impl>()) {}

HttpServer::~HttpServer() = default;

bool HttpServer::listen(std::string& error) {
    std::string bind_host = host_;
    if (bind_host.empty() || bind_host == "*") bind_host = "0.0.0.0";

    httplib::Server& server = impl_->server;

    // cpp-httplib's default socket options set SO_REUSEPORT, which lets a
    // second process bind this very address and silently split the traffic
    // with this one. An address already in use is an error the operator needs
    // to see, so set SO_REUSEADDR alone: enough to rebind over a socket left
    // in TIME_WAIT by a restart, not enough to hand the port to a twin.
    server.set_socket_options([](socket_t sock) {
        const int on = 1;
        ::setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));
    });

    // The knobs this class exposes, in the library's units.
    server.set_payload_max_length(max_body_bytes_);
    server.set_read_timeout(idle_timeout_, 0);
    server.set_keep_alive_timeout(idle_timeout_);
    server.new_task_queue = [this] { return new httplib::ThreadPool(max_connections_); };

    if (requested_port_ == 0) {
        const int port = server.bind_to_any_port(bind_host);
        if (port < 0) {
            error = "bind " + bind_host + ":0: could not bind (no address available, or not permitted)";
            return false;
        }
        bound_port_ = static_cast<std::uint16_t>(port);
    } else {
        if (!server.bind_to_port(bind_host, requested_port_)) {
            error = "bind " + bind_host + ":" + std::to_string(requested_port_) +
                    ": could not bind (address already in use, or not available on this host)";
            return false;
        }
        bound_port_ = requested_port_;
    }

    // ggmcp does its own routing - the MCP endpoint, /health, /, the metadata
    // document, and the 404/405 that follow - so the library is only asked to
    // hand over requests whose path it can match, and `.*` matches every path.
    // The methods listed are the ones cpp-httplib dispatches at all; a method
    // it does not dispatch (TRACE, CONNECT) reaches the error handler below.
    const httplib::Server::Handler route = [this](const httplib::Request& raw, httplib::Response& out) {
        dispatch(handler_, active_, raw, out);
    };
    server.Get(".*", route);
    server.Post(".*", route);
    server.Put(".*", route);
    server.Patch(".*", route);
    server.Delete(".*", route);
    server.Options(".*", route);

    // What the library could not route arrives here as a bare status line:
    // 400 for a method it does not dispatch, 404 for a path nothing claimed.
    // Both come with no body, which is how they are told apart from a response
    // ggmcp produced - every one of those carries a body, and is left alone.
    server.set_error_handler([this](const httplib::Request& raw, httplib::Response& out) {
        if (!out.body.empty() || (out.status != 400 && out.status != 404) || raw.path.empty()) {
            return httplib::Server::HandlerResponse::Unhandled;
        }
        dispatch(handler_, active_, raw, out);
        return httplib::Server::HandlerResponse::Handled;
    });

    return true;
}

int HttpServer::serve() {
    log_line("listening on http://" + host_ + ":" + std::to_string(bound_port_) + "/mcp");

    // The library's accept loop has no interval at which to notice a stop, so
    // the flag is watched from the side. install_signal_handlers() only raises
    // a sig_atomic_t - which is all a signal handler may do - and this thread
    // turns it into Server::stop(), which is not safe inside a handler. It
    // keeps retrying while the flag is up, because stop() before the accept
    // loop has started is a no-op.
    std::atomic<bool> serving{true};
    std::thread watchdog([this, &serving] {
        while (serving.load()) {
            if (stop_.load() || stop_requested()) impl_->server.stop();
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
    });

    const bool ok = impl_->server.listen_after_bind();

    serving.store(false);
    watchdog.join();

    log_line("stopped");
    return ok ? 0 : 1;
}

void install_signal_handlers() {
    struct sigaction action{};
    action.sa_handler = on_signal;
    sigemptyset(&action.sa_mask);
    action.sa_flags = 0;   // no SA_RESTART: let interrupted reads unwind cleanly
    ::sigaction(SIGINT, &action, nullptr);
    ::sigaction(SIGTERM, &action, nullptr);
    ::signal(SIGPIPE, SIG_IGN);   // writing to a closed socket must not kill us
}

bool stop_requested() { return g_stop_requested != 0; }

}  // namespace ggmcp
