int placeholder; //
#include "http_client.hpp"

#include "harness.hpp"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <sstream>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

namespace ggt {
namespace {

constexpr int kIoTimeoutSeconds = 15;

bool caseEquals(const std::string& lhs, const std::string& rhs) {
    if (lhs.size() != rhs.size()) {
        return false;
    }
    for (std::size_t i = 0; i < lhs.size(); ++i) {
        if (std::tolower(static_cast<unsigned char>(lhs[i])) !=
            std::tolower(static_cast<unsigned char>(rhs[i]))) {
            return false;
        }
    }
    return true;
}

std::string trim(const std::string& text) {
    const auto first = text.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) {
        return "";
    }
    const auto last = text.find_last_not_of(" \t\r\n");
    return text.substr(first, last - first + 1);
}

bool writeAll(int fd, const std::string& data) {
    std::size_t sent = 0;
    while (sent < data.size()) {
        const ssize_t written = ::send(fd, data.data() + sent, data.size() - sent, 0);
        if (written <= 0) {
            return false;
        }
        sent += static_cast<std::size_t>(written);
    }
    return true;
}

std::string toHex(std::size_t value) {
    char buffer[32];
    std::snprintf(buffer, sizeof(buffer), "%zx", value);
    return buffer;
}

} // namespace

bool HeaderLess::operator()(const std::string& lhs, const std::string& rhs) const {
    return std::lexicographical_compare(
        lhs.begin(), lhs.end(), rhs.begin(), rhs.end(),
        [](char lhsChar, char rhsChar) {
            return std::tolower(static_cast<unsigned char>(lhsChar)) <
                   std::tolower(static_cast<unsigned char>(rhsChar));
        });
}

std::string Reply::header(const std::string& name, const std::string& fallback) const {
    auto it = headers.find(name);
    return it == headers.end() ? fallback : it->second;
}

gg::Json Reply::json() const {
    gg::Json out;
    std::string error;
    gg::Json::tryParse(body, out, &error);
    return out;
}

bool Reply::bodyHas(const std::string& needle) const {
    return body.find(needle) != std::string::npos;
}

HttpClient::~HttpClient() {
    close();
}

bool HttpClient::open(const std::string& host, int port, std::string& error) {
    close();
    fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd_ < 0) {
        error = std::strerror(errno);
        return false;
    }
    timeval timeout {kIoTimeoutSeconds, 0};
    ::setsockopt(fd_, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    ::setsockopt(fd_, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));

    sockaddr_in address {};
    address.sin_family = AF_INET;
    address.sin_port = htons(static_cast<uint16_t>(port));
    if (::inet_pton(AF_INET, host.c_str(), &address.sin_addr) != 1) {
        error = "cannot parse the address '" + host + "'";
        close();
        return false;
    }
    if (::connect(fd_, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0) {
        error = "connect to " + host + ":" + std::to_string(port) + " failed: " +
                std::strerror(errno);
        close();
        return false;
    }
    host_ = host;
    port_ = port;
    rx_.clear();
    return true;
}

void HttpClient::close() {
    if (fd_ >= 0) {
        ::close(fd_);
    }
    fd_ = -1;
    rx_.clear();
}

bool HttpClient::fill(std::string& error) {
    char buffer[8192];
    const ssize_t read = ::recv(fd_, buffer, sizeof(buffer), 0);
    if (read < 0) {
        error = std::string("recv failed: ") + std::strerror(errno);
        close();
        return false;
    }
    if (read == 0) {
        error = "the server closed the connection";
        close();
        return false;
    }
    rx_.append(buffer, static_cast<std::size_t>(read));
    return true;
}

bool HttpClient::readReply(Reply& reply, std::string& error) {
    std::size_t boundary = rx_.find("\r\n\r\n");
    while (boundary == std::string::npos) {
        if (!fill(error)) {
            return false;
        }
        boundary = rx_.find("\r\n\r\n");
    }

    std::istringstream head(rx_.substr(0, boundary));
    std::string line;
    if (!std::getline(head, line)) {
        error = "the reply has no status line";
        return false;
    }
    if (!line.empty() && line.back() == '\r') {
        line.pop_back();
    }
    {
        std::istringstream status(line);
        status >> reply.version >> reply.status;
    }
    rx_.erase(0, boundary + 4);

    while (std::getline(head, line)) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        const std::size_t colon = line.find(':');
        if (colon == std::string::npos) {
            continue;
        }
        reply.headers[trim(line.substr(0, colon))] = trim(line.substr(colon + 1));
    }

    const bool chunked = caseEquals(reply.header("Transfer-Encoding"), "chunked");
    if (chunked) {
        std::string body;
        for (;;) {
            std::size_t lineEnd = rx_.find("\r\n");
            while (lineEnd == std::string::npos) {
                if (!fill(error)) {
                    return false;
                }
                lineEnd = rx_.find("\r\n");
            }
            const std::size_t size = std::strtoul(rx_.substr(0, lineEnd).c_str(), nullptr, 16);
            rx_.erase(0, lineEnd + 2);
            if (size == 0) {
                break;
            }
            while (rx_.size() < size + 2) {
                if (!fill(error)) {
                    return false;
                }
            }
            body.append(rx_, 0, size);
            rx_.erase(0, size + 2);
        }
        reply.body = body;
        return true;
    }

    const std::size_t wanted = static_cast<std::size_t>(
        std::strtoul(reply.header("Content-Length", "0").c_str(), nullptr, 10));
    while (rx_.size() < wanted) {
        if (!fill(error)) {
            return false;
        }
    }
    reply.body = rx_.substr(0, wanted);
    rx_.erase(0, wanted);
    return true;
}

bool HttpClient::request(const std::string& method, const std::string& target,
                         const std::string& body, const RequestOptions& options, Reply& reply,
                         std::string& error) {
    if (fd_ < 0 && !open(host_.empty() ? "127.0.0.1" : host_, port_, error)) {
        return false;
    }

    std::string out;
    if (!options.rawRequest.empty()) {
        out = options.rawRequest;
    } else {
        std::ostringstream stream;
        stream << method << " " << target << " HTTP/1.1\r\n";
        stream << "Host: "
               << (options.hostOverride.empty() ? host_ + ":" + std::to_string(port_)
                                                : options.hostOverride)
               << "\r\n";
        for (const auto& header : options.headers) {
            stream << header << "\r\n";
        }
        stream << "Connection: " << (options.closeConnection ? "close" : "keep-alive") << "\r\n";
        if (options.chunked) {
            stream << "Transfer-Encoding: chunked\r\n";
        } else if (!options.omitContentLength) {
            stream << "Content-Length: " << body.size() << "\r\n";
        }
        stream << "\r\n";
        if (options.chunked) {
            if (!body.empty()) {
                stream << toHex(body.size()) << "\r\n" << body << "\r\n";
            }
            stream << "0\r\n\r\n";
        } else {
            stream << body;
        }
        out = stream.str();
    }

    if (!writeAll(fd_, out)) {
        error = std::string("send failed: ") + std::strerror(errno);
        close();
        return false;
    }
    return readReply(reply, error);
}

Reply HttpClient::call(const std::string& method, const std::string& target,
                       const std::string& body, const RequestOptions& options) {
    Reply reply;
    std::string error;
    if (!request(method, target, body, options, reply, error)) {
        expect(__FILE__, __LINE__, method + " " + target + " reached the server", false, error);
        reply.status = 0;
        reply.body = error;
    }
    return reply;
}

Reply send(int port, const std::string& method, const std::string& target,
           const std::string& body, const RequestOptions& options) {
    HttpClient client;
    std::string error;
    if (!client.open("127.0.0.1", port, error)) {
        expect(__FILE__, __LINE__, "connect", false, error);
        Reply reply;
        reply.body = error;
        return reply;
    }
    return client.call(method, target, body, options);
}

std::string withSession(const std::string& sessionId) {
    return std::string("Mcp-Session-Id: ") + sessionId;
}

bool has(const std::vector<std::string>& headers, const std::string& name) {
    for (const auto& header : headers) {
        if (header.compare(0, name.size(), name) == 0) {
            return true;
        }
    }
    return false;
}

Reply postJson(int port, const std::string& body, const RequestOptions& options,
               const std::string& target) {
    RequestOptions withDefaults = options;
    if (!has(withDefaults.headers, "Content-Type:")) {
        withDefaults.headers.push_back("Content-Type: application/json");
    }
    if (!has(withDefaults.headers, "Accept:")) {
        withDefaults.headers.push_back("Accept: application/json, text/event-stream");
    }
    HttpClient client;
    std::string error;
    if (!client.open("127.0.0.1", port, error)) {
        expect(__FILE__, __LINE__, "connect", false, error);
        Reply reply;
        reply.body = error;
        return reply;
    }
    return client.call("POST", target, body, withDefaults);
}

Reply get(int port, const std::string& target, const RequestOptions& options) {
    HttpClient client;
    std::string error;
    if (!client.open("127.0.0.1", port, error)) {
        expect(__FILE__, __LINE__, "connect", false, error);
        Reply reply;
        reply.body = error;
        return reply;
    }
    return client.call("GET", target, {}, options);
}

Reply remove(int port, const std::string& target, const RequestOptions& options) {
    HttpClient client;
    std::string error;
    if (!client.open("127.0.0.1", port, error)) {
        expect(__FILE__, __LINE__, "connect", false, error);
        Reply reply;
        reply.body = error;
        return reply;
    }
    return client.call("DELETE", target, {}, options);
}

} // namespace ggt
