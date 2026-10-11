// ggmcp - a tiny Model Context Protocol server in dependency-free C++17.
//
// SPDX-License-Identifier: MIT

#include "ggmcp/http_server.hpp"

#include "ggmcp/log.hpp"

#include <atomic>
#include <cctype>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <thread>

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

namespace ggmcp {
namespace {

// sig_atomic_t lives in the global namespace on every libc we target; std::
// sig_atomic_t is optional in the standard, so do not rely on it.
volatile sig_atomic_t g_stop_requested = 0;

void on_signal(int) { g_stop_requested = 1; }

std::string to_lower(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

bool is_space(char c) {
    return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}

std::string trim(std::string s) {
    while (!s.empty() && is_space(s.back())) s.pop_back();
    std::size_t begin = 0;
    while (begin < s.size() && is_space(s[begin])) ++begin;
    return s.substr(begin);
}

constexpr std::size_t kMaxHeaderBytes = 64u * 1024u;

const char* reason_phrase(int status) {
    switch (status) {
        case 200: return "OK";
        case 201: return "Created";
        case 202: return "Accepted";
        case 204: return "No Content";
        case 400: return "Bad Request";
        case 401: return "Unauthorized";
        case 403: return "Forbidden";
        case 404: return "Not Found";
        case 405: return "Method Not Allowed";
        case 406: return "Not Acceptable";
        case 408: return "Request Timeout";
        case 409: return "Conflict";
        case 411: return "Length Required";
        case 413: return "Content Too Large";
        case 415: return "Unsupported Media Type";
        case 431: return "Request Header Fields Too Large";
        case 500: return "Internal Server Error";
        case 501: return "Not Implemented";
        case 503: return "Service Unavailable";
        default:  return "Unknown";
    }
}

std::string http_date() {
    const std::time_t now = std::time(nullptr);
    std::tm utc{};
    ::gmtime_r(&now, &utc);
    char buf[64];
    if (std::strftime(buf, sizeof(buf), "%a, %d %b %Y %H:%M:%S GMT", &utc) == 0) return "";
    return buf;
}

/// Decodes a chunked transfer-encoded body. `complete` is false when more
/// bytes are still required to finish the stream.
bool decode_chunked(const std::string& data, std::string& body, bool& complete) {
    std::size_t pos = 0;
    body.clear();
    for (;;) {
        const std::size_t line_end = data.find("\r\n", pos);
        if (line_end == std::string::npos) { complete = false; return true; }

        std::size_t size_end = line_end;
        for (std::size_t i = pos; i < line_end; ++i) {
            // Strip chunk extensions ("5;name=value").
            if (data[i] == ';') { size_end = i; break; }
        }

        std::size_t size = 0;
        for (std::size_t i = pos; i < size_end; ++i) {
            const char c = data[i];
            std::size_t digit = 0;
            if (c >= '0' && c <= '9')      digit = static_cast<std::size_t>(c - '0');
            else if (c >= 'a' && c <= 'f') digit = static_cast<std::size_t>(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F') digit = static_cast<std::size_t>(c - 'A' + 10);
            else return false;   // not hex
            size = size * 16u + digit;
            if (size > (1u << 28)) return false;   // absurdly large chunk
        }

        const std::size_t next = line_end + 2 + size;
        if (data.size() < next + 2) { complete = false; return true; }
        if (data.compare(next, 2, "\r\n") != 0) return false;   // missing chunk terminator
        if (size == 0) { complete = true; return true; }
        body.append(data, line_end + 2, size);
        pos = next + 2;
    }
}

bool write_all(int fd, const char* data, std::size_t size, int timeout_ms) {
    std::size_t sent = 0;
    while (sent < size) {
        struct pollfd pfd{};
        pfd.fd = fd;
        pfd.events = POLLOUT;
        const int pr = ::poll(&pfd, 1, timeout_ms);
        if (pr < 0) { if (errno == EINTR) continue; return false; }
        if (pr == 0) return false;   // slow client; drop it
        const ssize_t n = ::write(fd, data + sent, size - sent);
        if (n < 0) { if (errno == EINTR) continue; return false; }
        if (n == 0) return false;
        sent += static_cast<std::size_t>(n);
    }
    return true;
}

bool write_all(int fd, const std::string& data, int timeout_ms) {
    return write_all(fd, data.data(), data.size(), timeout_ms);
}

}  // namespace

std::string Request::header(const std::string& name) const {
    const auto it = headers.find(to_lower(name));
    return it == headers.end() ? std::string() : it->second;
}

bool Request::has_header(const std::string& name) const {
    return headers.count(to_lower(name)) != 0;
}

void Response::set(const std::string& name, const std::string& value) {
    for (auto& kv : headers) {
        if (to_lower(kv.first) == to_lower(name)) { kv.second = value; return; }
    }
    headers.emplace_back(name, value);
}

std::string Response::serialize() const {
    std::string out;
    out.reserve(body.size() + 256);
    out += "HTTP/1.1 ";
    out += std::to_string(status);
    out += ' ';
    out += reason_phrase(status);
    out += "\r\n";

    const std::string date = http_date();
    if (!date.empty()) out += "Date: " + date + "\r\n";
    out += "Server: ggmcp/0.1.0\r\n";

    // 1xx/204/304 must not carry a body, and advertising Content-Length: 0 on
    // them makes some strict clients complain, so omit it entirely.
    const bool bodyless = status < 200 || status == 204 || status == 304;

    if (!content_type.empty() && !bodyless) out += "Content-Type: " + content_type + "\r\n";
    if (!bodyless) out += "Content-Length: " + std::to_string(body.size()) + "\r\n";
    out += std::string("Connection: ") + (close ? "close" : "keep-alive") + "\r\n";

    for (const auto& kv : headers) {
        const std::string key = to_lower(kv.first);
        if (key == "content-length" || key == "connection" || key == "content-type" || key == "date" ||
            key == "server") {
            continue;   // already emitted by us
        }
        out += kv.first + ": " + kv.second + "\r\n";
    }

    out += "\r\n";
    if (!bodyless) out += body;
    return out;
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

Response Response::failure(int status, const std::string& reason) {
    return Response::text(status, std::string(reason_phrase(status)) + ": " + reason + "\n");
}

// ---------------------------------------------------------------------------
// HttpServer
// ---------------------------------------------------------------------------

HttpServer::HttpServer(std::string host, std::uint16_t port, Handler handler)
    : host_(std::move(host)), requested_port_(port), handler_(std::move(handler)) {}

HttpServer::~HttpServer() {
    if (listen_fd_ >= 0) {
        ::close(listen_fd_);
        listen_fd_ = -1;
    }
}

bool HttpServer::listen(std::string& error) {
    listen_fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd_ < 0) {
        error = std::string("socket: ") + std::strerror(errno);
        return false;
    }

    const int on = 1;
    ::setsockopt(listen_fd_, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));

    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(requested_port_);
    if (host_.empty() || host_ == "0.0.0.0" || host_ == "*") {
        address.sin_addr.s_addr = htonl(INADDR_ANY);
    } else if (::inet_pton(AF_INET, host_.c_str(), &address.sin_addr) != 1) {
        error = "could not parse bind address '" + host_ + "' (expected IPv4, e.g. 127.0.0.1)";
        return false;
    }

    if (::bind(listen_fd_, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0) {
        error = std::string("bind ") + host_ + ":" + std::to_string(requested_port_) + ": " + std::strerror(errno);
        return false;
    }

    if (::listen(listen_fd_, 64) != 0) {
        error = std::string("listen: ") + std::strerror(errno);
        return false;
    }

    sockaddr_in bound{};
    socklen_t len = sizeof(bound);
    if (::getsockname(listen_fd_, reinterpret_cast<sockaddr*>(&bound), &len) == 0) {
        bound_port_ = ntohs(bound.sin_port);
    } else {
        bound_port_ = requested_port_;
    }
    return true;
}

int HttpServer::serve() {
    log_line("listening on http://" + host_ + ":" + std::to_string(bound_port_) + "/mcp");

    while (!stop_.load() && !stop_requested()) {
        struct pollfd pfd{};
        pfd.fd = listen_fd_;
        pfd.events = POLLIN;
        const int pr = ::poll(&pfd, 1, 200);
        if (pr < 0) {
            if (errno == EINTR) continue;
            log_line(std::string("poll: ") + std::strerror(errno));
            return 1;
        }
        if (pr == 0) continue;   // poll timeout: re-check the stop flag

        sockaddr_in peer{};
        socklen_t len = sizeof(peer);
        const int fd = ::accept(listen_fd_, reinterpret_cast<sockaddr*>(&peer), &len);
        if (fd < 0) {
            if (errno == EINTR || errno == ECONNABORTED || errno == EAGAIN || errno == EWOULDBLOCK) continue;
            log_line(std::string("accept: ") + std::strerror(errno));
            return 1;
        }

        char host[INET_ADDRSTRLEN] = {0};
        ::inet_ntop(AF_INET, &peer.sin_addr, host, sizeof(host));
        std::string peer_name = std::string(host) + ":" + std::to_string(ntohs(peer.sin_port));

        if (active_.load() >= max_connections_) {
            log_line("rejecting " + peer_name + ": at the " + std::to_string(max_connections_) +
                     " connection limit");
            const Response busy = Response::failure(503, "connection limit reached");
            write_all(fd, busy.serialize(), 2000);
            ::close(fd);
            continue;
        }

        const int nodelay = 1;
        ::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &nodelay, sizeof(nodelay));

        ++active_;
        std::thread([this, fd, peer = std::move(peer_name)] {
            serve_connection(fd, std::move(peer));
            ::close(fd);
            --active_;
        }).detach();
    }

    // Connection loops poll the stop flag, so this normally drains at once.
    for (int wait_ms = 0; wait_ms < 3000 && active_.load() > 0; wait_ms += 50) {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    log_line("stopped");
    return 0;
}

void HttpServer::serve_connection(int fd, std::string peer) {
    std::string buffer;
    buffer.reserve(16u * 1024u);
    char chunk[16384];
    std::size_t scan_from = 0;

    for (;;) {
        if (stopping() || stop_requested()) return;

        // --- 1. Read a full request head (CRLF CRLF). ----------------------
        std::size_t header_end = buffer.find("\r\n\r\n", scan_from);
        while (header_end == std::string::npos) {
            struct pollfd pfd{};
            pfd.fd = fd;
            pfd.events = POLLIN;
            const int pr = ::poll(&pfd, 1, idle_timeout_ * 1000);
            if (pr == 0) {
                log_debug("idle timeout from " + peer);
                return;
            }
            if (pr < 0) {
                if (errno == EINTR) continue;
                return;
            }
            const ssize_t n = ::read(fd, chunk, sizeof(chunk));
            if (n == 0) return;   // client hung up between requests
            if (n < 0) {
                if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) continue;
                return;
            }
            buffer.append(chunk, static_cast<std::size_t>(n));
            scan_from = buffer.size() > static_cast<std::size_t>(n) ? buffer.size() - static_cast<std::size_t>(n) : 0;
            header_end = buffer.find("\r\n\r\n", scan_from);
            if (buffer.size() > kMaxHeaderBytes) {
                write_all(fd, Response::failure(431, "request headers too large").serialize(), 2000);
                return;
            }
        }
        scan_from = header_end + 4;

        // --- 2. Parse the request line and headers. ------------------------
        Request request;
        request.peer = peer;
        {
            std::size_t line_start = 0;
            const std::size_t head_size = header_end;
            const std::size_t first_end = buffer.find("\r\n", 0);
            if (first_end == std::string::npos || first_end > head_size) {
                write_all(fd, Response::failure(400, "malformed request line").serialize(), 2000);
                return;
            }
            const std::string request_line = buffer.substr(0, first_end);
            {
                const std::size_t sp1 = request_line.find(' ');
                const std::size_t sp2 = sp1 == std::string::npos ? std::string::npos
                                                                 : request_line.find(' ', sp1 + 1);
                if (sp1 == std::string::npos || sp2 == std::string::npos) {
                    write_all(fd, Response::failure(400, "malformed request line").serialize(), 2000);
                    return;
                }
                request.method = to_lower(request_line.substr(0, sp1));
                request.target = trim(request_line.substr(sp1 + 1, sp2 - sp1 - 1));
                request.version = trim(request_line.substr(sp2 + 1));
            }
            const std::size_t q = request.target.find('?');
            request.path = q == std::string::npos ? request.target : request.target.substr(0, q);
            request.query = q == std::string::npos ? std::string() : request.target.substr(q + 1);

            line_start = first_end + 2;
            while (line_start < head_size) {
                std::size_t line_end = buffer.find("\r\n", line_start);
                if (line_end == std::string::npos || line_end > head_size) line_end = head_size;
                const std::string line = buffer.substr(line_start, line_end - line_start);
                line_start = line_end + 2;
                if (line.empty()) continue;
                const std::size_t colon = line.find(':');
                if (colon == std::string::npos) continue;   // ignore unparseable headers
                request.headers[to_lower(trim(line.substr(0, colon)))] = trim(line.substr(colon + 1));
            }

            const std::string connection = to_lower(request.header("connection"));
            if (request.version == "HTTP/1.1") request.keep_alive = connection != "close";
            else                                request.keep_alive = connection == "keep-alive";
        }

        // --- 3. Read the body (Content-Length or chunked). -----------------
        std::size_t body_start = header_end + 4;
        const std::string transfer_encoding = to_lower(request.header("transfer-encoding"));
        bool body_complete = false;

        if (transfer_encoding.find("chunked") != std::string::npos) {
            while (!body_complete) {
                const std::string tail(buffer.data() + body_start, buffer.size() - body_start);
                bool complete = false;
                if (!decode_chunked(tail, request.body, complete)) {
                    write_all(fd, Response::failure(400, "malformed chunked body").serialize(), 2000);
                    return;
                }
                if (complete) {
                    body_complete = true;
                    // Estimate the consumed prefix; the connection closes after the
                    // response, so exact framing of leftovers does not matter here.
                    scan_from = buffer.size();
                } else {
                    struct pollfd pfd{};
                    pfd.fd = fd;
                    pfd.events = POLLIN;
                    const int pr = ::poll(&pfd, 1, idle_timeout_ * 1000);
                    if (pr == 0) {
                        write_all(fd, Response::failure(408, "timed out reading chunked body").serialize(), 2000);
                        return;
                    }
                    if (pr < 0 && errno != EINTR) return;
                    const ssize_t n = ::read(fd, chunk, sizeof(chunk));
                    if (n == 0) {
                        log_debug("client closed mid-body");
                        return;
                    }
                    if (n < 0 && (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)) continue;
                    if (n < 0) return;
                    if (buffer.size() - body_start + static_cast<std::size_t>(n) > max_body_bytes_) {
                        write_all(fd, Response::failure(413, "request body too large").serialize(), 2000);
                        return;
                    }
                    buffer.append(chunk, static_cast<std::size_t>(n));
                }
            }
        } else {
            const std::string length_text = trim(request.header("content-length"));
            std::size_t want = 0;
            if (!length_text.empty()) {
                char* end = nullptr;
                const unsigned long long parsed = std::strtoull(length_text.c_str(), &end, 10);
                if (!end || *end != '\0') {
                    write_all(fd, Response::failure(400, "invalid Content-Length").serialize(), 2000);
                    return;
                }
                want = static_cast<std::size_t>(parsed);
                if (want > max_body_bytes_) {
                    write_all(fd, Response::failure(413, "request body too large").serialize(), 2000);
                    return;
                }
            }
            while (buffer.size() < body_start + want) {
                struct pollfd pfd{};
                pfd.fd = fd;
                pfd.events = POLLIN;
                const int pr = ::poll(&pfd, 1, idle_timeout_ * 1000);
                if (pr == 0) {
                    write_all(fd, Response::failure(408, "timed out reading body").serialize(), 2000);
                    return;
                }
                if (pr < 0 && errno != EINTR) return;
                const ssize_t n = ::read(fd, chunk, sizeof(chunk));
                if (n == 0) {
                    log_debug("client closed mid-body");
                    return;
                }
                if (n < 0 && (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)) continue;
                if (n < 0) return;
                buffer.append(chunk, static_cast<std::size_t>(n));
            }
            request.body = buffer.substr(body_start, want);
            body_complete = true;
        }

        // --- 4. Dispatch and answer. --------------------------------------
        Response response;
        try {
            response = handler_(request);
        } catch (const std::exception& e) {
            log_line(std::string("handler threw for ") + request.method + " " + request.path + ": " + e.what());
            response = Response::failure(500, "internal error");
        }
        if (!request.keep_alive) response.close = true;
        if (response.close) response.set("Connection", "close");

        const bool ok = write_all(fd, response.serialize(), 10000);
        log_debug(request.method + " " + request.path + " -> " + std::to_string(response.status) +
                  (ok ? "" : " (write failed)"));
        if (!ok || response.close || !request.keep_alive) return;

        // --- 5. Drop the consumed request and keep the connection open. ----
        if (transfer_encoding.find("chunked") != std::string::npos) return;   // see note above
        buffer.erase(0, body_start + request.body.size());
        scan_from = 0;
    }
}

void install_signal_handlers() {
    struct sigaction action{};
    action.sa_handler = on_signal;
    sigemptyset(&action.sa_mask);
    action.sa_flags = 0;   // no SA_RESTART: let poll() return EINTR and unwind cleanly
    ::sigaction(SIGINT, &action, nullptr);
    ::sigaction(SIGTERM, &action, nullptr);
    ::signal(SIGPIPE, SIG_IGN);   // writing to a closed socket must not kill us
}

bool stop_requested() { return g_stop_requested != 0; }

}  // namespace ggmcp
