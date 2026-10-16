#include "net.hpp"

#include <netdb.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cctype>
#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <cstdio>
#include <sstream>

namespace e2e {
namespace {

std::string lowered(std::string text) {
    for (char& c : text) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return text;
}

long long clock_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

constexpr int kTimeoutMs = 15000;

std::string hex(std::size_t value) {
    std::ostringstream out;
    out << std::hex << value;
    return out.str();
}

}  // namespace

bool Message::has(const std::string& name) const {
    const std::string want = lowered(name);
    for (const Header& header : headers) {
        if (lowered(header.name) == want) return true;
    }
    return false;
}

std::string Message::header(const std::string& name) const {
    const std::string want = lowered(name);
    for (const Header& header : headers) {
        if (lowered(header.name) == want) return header.value;
    }
    return std::string();
}

std::size_t Message::count(const std::string& name) const {
    const std::string want = lowered(name);
    std::size_t seen = 0;
    for (const Header& header : headers) {
        if (lowered(header.name) == want) ++seen;
    }
    return seen;
}

Json Message::json() const {
    try {
        return Json::parse(body);
    } catch (const std::exception& problem) {
        throw Failure(std::string("the body is not JSON: ") + problem.what() + "\n  body: " + body);
    }
}

std::string Call::wire() const {
    std::string out = method + " " + path + " HTTP/1.1\r\n";
    bool have_host = false;
    bool have_length = false;
    bool have_encoding = false;
    for (const Header& header : headers) {
        const std::string name = lowered(header.name);
        if (name == "host") have_host = true;
        if (name == "content-length") have_length = true;
        if (name == "transfer-encoding") have_encoding = true;
        out += header.name + ": " + header.value + "\r\n";
    }
    if (!have_host) out += "Host: " + (authority.empty() ? std::string("127.0.0.1") : authority) + "\r\n";
    if (!accept.empty()) out += "Accept: " + accept + "\r\n";
    if (send_content_type && !content_type.empty()) out += "Content-Type: " + content_type + "\r\n";
    if (!connection.empty()) out += "Connection: " + connection + "\r\n";

    if (!chunk_body.empty() || !chunks.empty()) {
        if (!have_encoding) out += "Transfer-Encoding: chunked\r\n";
        out += "\r\n";
        if (!chunks.empty()) {
            for (const std::string& chunk : chunks) {
                out += hex(chunk.size()) + "\r\n" + chunk + "\r\n";
            }
        } else {
            const std::string size = chunk_size.empty() ? hex(chunk_body.size()) : chunk_size;
            out += size + "\r\n" + chunk_body + "\r\n";
        }
        out += "0\r\n\r\n";
        return out;
    }

    const std::string payload = body;

    if (have_length) {
        out += "\r\n";
    } else if (send_content_length) {
        out += "Content-Length: " + std::to_string(payload.size()) + "\r\n\r\n";
    } else {
        out += "\r\n";
    }
    out += payload;
    return out;
}

Call post(const std::string& path, const std::string& body) {
    Call call("POST", path);
    call.body = body;
    return call;
}

Call get(const std::string& path) {
    Call call("GET", path);
    call.send_content_type = false;
    call.content_type.clear();
    call.send_content_length = false;
    return call;
}

Call del(const std::string& path) {
    Call call("DELETE", path);
    call.send_content_type = false;
    call.content_type.clear();
    call.send_content_length = false;
    return call;
}

Call verb(const std::string& method, const std::string& path) {
    Call call(method, path);
    call.send_content_type = false;
    call.content_type.clear();
    call.send_content_length = false;
    return call;
}

Connection::Connection(const std::string& host, int port) : host_(host), port_(port) {
    addrinfo hints {};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    addrinfo* answer = nullptr;
    const std::string service = std::to_string(port);
    const int code = ::getaddrinfo(host.c_str(), service.c_str(), &hints, &answer);
    if (code != 0) throw Failure(std::string("cannot resolve ") + host + ": " + gai_strerror(code));

    // A socket that has failed to connect once cannot be tried again, so every
    // attempt gets a fresh one until the server is listening.
    const long long until = clock_ms() + kTimeoutMs;
    int last = 0;
    for (;;) {
        const int candidate = ::socket(answer->ai_family, answer->ai_socktype, answer->ai_protocol);
        if (candidate < 0) {
            last = errno;
            break;
        }
        if (::connect(candidate, answer->ai_addr, answer->ai_addrlen) == 0) {
            fd_ = candidate;
            break;
        }
        last = errno;
        ::close(candidate);
        if (clock_ms() > until) break;
        ::usleep(20 * 1000);
    }
    const int reason = last;
    ::freeaddrinfo(answer);

    if (fd_ < 0) {
        throw Failure("cannot connect to " + host + ":" + service + ": " + std::strerror(reason));
    }
}

Connection::~Connection() { close(); }

void Connection::close() {
    if (fd_ >= 0) ::close(fd_);
    fd_ = -1;
    eof_ = true;
}

void Connection::send(const Call& call) {
    if (fd_ < 0) throw Failure("the connection is closed");
    Call copy = call;
    if (copy.authority.empty()) copy.authority = host_ + ":" + std::to_string(port_);
    const std::string bytes = copy.wire();
    std::size_t sent = 0;
    const long long until = clock_ms() + kTimeoutMs;
    while (sent < bytes.size()) {
        if (clock_ms() > until) throw Failure("the request never went out in time");
        const ssize_t n = ::send(fd_, bytes.data() + sent, bytes.size() - sent, 0);
        if (n > 0) {
            sent += static_cast<std::size_t>(n);
            continue;
        }
        if (n < 0 && errno == EINTR) continue;
        throw Failure(std::string("the request did not go out: ") + std::strerror(errno));
    }
}

void Connection::feed() {
    if (eof_) return;
    struct pollfd pfd;
    pfd.fd = fd_;
    pfd.events = POLLIN;
    pfd.revents = 0;
    const int ready = ::poll(&pfd, 1, kTimeoutMs);
    if (ready == 0) throw Failure("the server sent nothing for 15s");
    if (ready < 0) {
        if (errno == EINTR) return;
        throw Failure(std::string("poll: ") + std::strerror(errno));
    }
    char buffer[16384];
    const ssize_t n = ::read(fd_, buffer, sizeof(buffer));
    if (n == 0) {
        eof_ = true;
        return;
    }
    if (n < 0) {
        if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) return;
        throw Failure(std::string("read: ") + std::strerror(errno));
    }
    in_.append(buffer, static_cast<std::size_t>(n));
}

std::string Connection::need(std::size_t bytes) {
    const long long until = clock_ms() + kTimeoutMs;
    while (in_.size() < bytes) {
        if (eof_) throw Failure("the server closed with " + std::to_string(in_.size()) +
                                " byte(s) of a " + std::to_string(bytes) + "-byte body");
        feed();
        if (clock_ms() > until) throw Failure("the body never arrived");
    }
    std::string out = in_.substr(0, bytes);
    in_.erase(0, bytes);
    return out;
}

std::string Connection::line() {
    for (;;) {
        const std::size_t at = in_.find("\r\n");
        if (at != std::string::npos) {
            std::string out = in_.substr(0, at);
            in_.erase(0, at + 2);
            return out;
        }
        if (eof_) throw Failure("the server closed inside a line");
        feed();
    }
}

Message Connection::response() {
    Message message;
    std::size_t at = std::string::npos;
    const long long until = clock_ms() + kTimeoutMs;
    for (;;) {
        at = in_.find("\r\n\r\n");
        if (at != std::string::npos) break;
        if (eof_) throw Failure("the server closed inside the headers");
        if (clock_ms() > until) throw Failure("the headers never arrived");
        feed();
    }

    const std::string head = in_.substr(0, at);
    in_.erase(0, at + 4);
    message.raw = head + "\r\n\r\n";

    const std::size_t line_end = head.find("\r\n");
    const std::string status_line = head.substr(0, line_end);
    const std::size_t first = status_line.find(' ');
    const std::size_t second = status_line.find(' ', first == std::string::npos ? 0 : first + 1);
    if (first == std::string::npos) throw Failure("not a status line: " + status_line);
    message.status = std::atoi(status_line.substr(first + 1, second - first - 1).c_str());
    if (second != std::string::npos) {
        message.reason = trim(status_line.substr(second + 1));
    }

    std::size_t at_line = line_end == std::string::npos ? head.size() : line_end + 2;
    while (at_line < head.size()) {
        std::size_t end = head.find("\r\n", at_line);
        if (end == std::string::npos) end = head.size();
        const std::string field = head.substr(at_line, end - at_line);
        const std::size_t colon = field.find(':');
        if (colon != std::string::npos) {
            message.headers.push_back(Header{trim(field.substr(0, colon)), trim(field.substr(colon + 1))});
        }
        at_line = end + 2;
    }

    const std::string encoding = lowered(message.header("Transfer-Encoding"));
    if (encoding.find("chunked") != std::string::npos) {
        for (;;) {
            const std::string size = line();
            const long long want = std::strtoll(size.c_str(), nullptr, 16);
            if (want <= 0) {
                line();  // the empty line that closes the last chunk
                break;
            }
            message.body += need(static_cast<std::size_t>(want));
            need(2);  // the CRLF that follows a chunk
        }
        message.raw += "[chunked] " + message.body;
        return message;
    }

    if (message.has("Content-Length")) {
        const long long want = std::atoll(message.header("Content-Length").c_str());
        if (want > 0) message.body = need(static_cast<std::size_t>(want));
        message.raw += message.body;
        return message;
    }

    if (message.status == 204 || message.status == 304 ||
        (message.status >= 100 && message.status < 200)) {
        return message;
    }

    while (!eof_) feed();
    message.body = in_;
    in_.clear();
    message.raw += message.body;
    return message;
}

Message Connection::request(const Call& call) {
    send(call);
    return response();
}

Message request(const std::string& host, int port, const Call& call) {
    Connection connection(host, port);
    Message message = connection.request(call);
    connection.close();
    return message;
}

}  // namespace e2e
