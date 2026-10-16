// A deliberately small HTTP/1.1 client over a blocking socket.
//
// It speaks the wire by hand so the tests exercise what a real MCP client
// sends: keep-alive, chunked bodies, bare LF, missing headers and so on.

#pragma once

#include <map>
#include <string>
#include <vector>

#include "gg/json.hpp"

namespace ggt {

struct HeaderLess {
    bool operator()(const std::string& lhs, const std::string& rhs) const;
};

struct Reply {
    int status = 0;
    std::string version;
    std::map<std::string, std::string, HeaderLess> headers;
    std::string body;

    std::string header(const std::string& name, const std::string& fallback = {}) const;
    gg::Json json() const;
    bool bodyHas(const std::string& needle) const;
    bool bodyHas(const char* needle) const { return bodyHas(std::string(needle)); }
};

struct RequestOptions {
    std::vector<std::string> headers; // "Name: value"
    bool closeConnection = false;
    bool chunked = false;
    bool omitContentLength = false;
    std::string hostOverride; // empty means host:port of the connection
    std::string rawRequest;   // sent verbatim when set
};

class HttpClient {
public:
    HttpClient() = default;
    ~HttpClient();

    HttpClient(const HttpClient&) = delete;
    HttpClient& operator=(const HttpClient&) = delete;

    bool open(const std::string& host, int port, std::string& error);
    void close();
    bool isOpen() const { return fd_ >= 0; }
    int port() const { return port_; }

    bool request(const std::string& method, const std::string& target, const std::string& body,
                 const RequestOptions& options, Reply& reply, std::string& error);

    // Records a test failure when the transport itself fails.
    Reply call(const std::string& method, const std::string& target, const std::string& body = {},
               const RequestOptions& options = {});

private:
    bool fill(std::string& error);
    bool readReply(Reply& reply, std::string& error);

    int fd_ = -1;
    int port_ = 0;
    std::string host_;
    std::string rx_;
};

// Fresh connection per call, with the headers an MCP client sends.
Reply postJson(int port, const std::string& body, const RequestOptions& options = {},
             const std::string& target = "/mcp");
Reply get(int port, const std::string& target, const RequestOptions& options = {});
Reply remove(int port, const std::string& target, const RequestOptions& options = {});

std::string withSession(const std::string& sessionId);

} // namespace ggt
