// Backend-agnostic HTTP types.
//
// The server only ever sees gg::http::Request / gg::http::Response and routes
// through gg::http::Router. A transport backend (cpp-httplib today) translates
// its own wire types into these, so it can be swapped without touching the MCP
// layer. See gg/backends.hpp.

#pragma once

#include <functional>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "gg/json.hpp"

namespace gg {
namespace http {

struct CaseInsensitiveLess {
    bool operator()(const std::string& lhs, const std::string& rhs) const;
};

using Headers = std::map<std::string, std::string, CaseInsensitiveLess>;
using Query = std::vector<std::pair<std::string, std::string>>;

struct Request {
    std::string method;   // upper case, e.g. "POST"
    std::string target;   // raw request target, e.g. "/mcp?a=b"
    std::string path;     // percent-decoded path, e.g. "/mcp"
    std::string body;
    std::string remoteAddress;
    int remotePort = 0;
    Headers headers;
    Query query;

    std::string header(const std::string& name, const std::string& fallback = {}) const;
    bool hasHeader(const std::string& name) const;
    std::string param(const std::string& name, const std::string& fallback = {}) const;
    bool hasParam(const std::string& name) const;
    std::string scheme() const;
    std::string authority() const; // Host header, falling back to the bound address
};

struct Response {
    int status = 200;
    std::string body;
    Headers headers;

    void setHeader(const std::string& name, const std::string& value);
    void setContentType(const std::string& value);
    std::string header(const std::string& name, const std::string& fallback = {}) const;

    static Response text(int status, std::string body);
    static Response json(int status, const gg::Json& body);
    static Response empty(int status);
};

using Handler = std::function<Response(const Request&)>;

// Exact path routing, shared by every HTTP backend.
class Router {
public:
    void add(const std::string& method, const std::string& path, Handler handler);
    bool knowsPath(const std::string& path) const;
    std::vector<std::string> methodsFor(const std::string& path) const;
    std::string allowHeaderFor(const std::string& path) const;
    Response dispatch(const Request& request) const;

    static Response errorBody(int status, const std::string& message);

private:
    struct Route {
        std::string method;
        std::string path;
        Handler handler;
    };

    std::vector<Route> routes_;
};

// The HTTP backend interface. A backend turns a wire level request into a
// gg::http::Request, hands it to the router and writes back the response.
struct IServer {
    virtual ~IServer() = default;

    virtual std::string name() const = 0;
    virtual void setRouter(std::shared_ptr<const Router> router) = 0;
    virtual void setLogger(std::function<void(const std::string&)> logger) = 0;

    // listen() binds and serves on the calling thread. bind() only reserves the
    // socket so that the kernel assigned port is readable before serving.
    virtual bool bind(const std::string& host, int port, std::string& error) = 0;
    virtual bool listen(std::string& error) = 0;
    virtual void stop() = 0;
    virtual int port() const = 0;
};

std::string urlDecode(std::string_view value);
std::string statusText(int status);

} // namespace http

namespace backends {
std::unique_ptr<http::IServer> makeHttpServer();
} // namespace backends

} // namespace gg
