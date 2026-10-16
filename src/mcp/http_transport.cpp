#include "gg/mcp/server.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <sstream>
#include <thread>

namespace gg::mcp {
namespace {

std::chrono::steady_clock::time_point gStartedAt = std::chrono::steady_clock::now();

long long uptimeSeconds() {
    return std::chrono::duration_cast<std::chrono::seconds>(
               std::chrono::steady_clock::now() - gStartedAt)
        .count();
}

std::string htmlEscape(const std::string& text) {
    std::string out;
    out.reserve(text.size());
    for (const char c : text) {
        switch (c) {
        case '&': out += "&amp;"; break;
        case '<': out += "&lt;"; break;
        case '>': out += "&gt;"; break;
        case '"': out += "&quot;"; break;
        default: out.push_back(c);
        }
    }
    return out;
}

bool originAllowed(const http::Request& request, const Options& options) {
    const std::string origin = request.header("Origin");
    if (origin.empty()) {
        return true;
    }
    if (!options.strictOrigin && options.allowedOrigins.empty()) {
        return true;
    }
    const std::string own = request.scheme() + "://" + request.authority();
    if (origin == own) {
        return true;
    }
    for (const auto& allowed : options.allowedOrigins) {
        if (allowed == origin || allowed == "*") {
            return true;
        }
    }
    return false;
}

// CORS: a browser only reads the reply when the headers say so, and it sends an
// OPTIONS preflight first. The origin is echoed rather than '*' so that clients
// sending credentials keep working.
constexpr const char* kAllowedMethods = "POST, DELETE, OPTIONS";
constexpr const char* kExposeHeaders = "Mcp-Session-Id, MCP-Protocol-Version";
constexpr const char* kDefaultAllowHeaders =
    "Authorization, Content-Type, Accept, Mcp-Session-Id, MCP-Protocol-Version, Last-Event-ID";

void applyCors(const http::Request& request, const Options& options, http::Response& response) {
    if (!options.cors) {
        return;
    }
    const std::string origin = request.header("Origin");
    if (origin.empty() || !originAllowed(request, options)) {
        return;
    }
    response.setHeader("Access-Control-Allow-Origin", origin);
    response.setHeader("Vary", "Origin");
    response.setHeader("Access-Control-Expose-Headers", kExposeHeaders);
}

const char* const kCorsDenied = "the Origin header is not allowed, use --allow-origin to allow it";

http::Response preflight(const http::Request& request, const Options& options) {
    if (!options.cors) {
        return http::Router::errorBody(405, "this server does not answer CORS preflights");
    }
    if (!originAllowed(request, options)) {
        return http::Router::errorBody(403, kCorsDenied);
    }
    http::Response response = http::Response::empty(204);
    response.setHeader("Access-Control-Allow-Origin", request.header("Origin"));
    response.setHeader("Vary", "Origin");
    response.setHeader("Access-Control-Allow-Methods", kAllowedMethods);
    const std::string requested = request.header("Access-Control-Request-Headers");
    response.setHeader("Access-Control-Allow-Headers",
                       requested.empty() ? kDefaultAllowHeaders : requested);
    response.setHeader("Access-Control-Max-Age", "600");
    return response;
}

http::Response finish(const http::Request& request, const Options& options,
                      http::Response response) {
    applyCors(request, options, response);
    return response;
}

http::Response fromOutbound(const Outbound& outbound) {
    http::Response response;
    response.status = outbound.status;
    if (outbound.hasBody) {
        response.body = outbound.payload.dump();
        if (!outbound.contentType.empty()) {
            response.setHeader("Content-Type", outbound.contentType);
        }
    }
    if (!outbound.sessionId.empty()) {
        response.setHeader(protocol::kSessionIdHeader, outbound.sessionId);
    }
    return response;
}

std::string describeTools(const ToolRegistry& tools) {
    std::string names;
    for (const auto& name : tools.names()) {
        if (!names.empty()) {
            names += ", ";
        }
        names += name;
    }
    return names;
}

} // namespace

void logLine(const Options& options, const std::string& line) {
    if (options.quiet) {
        return;
    }
    const std::time_t now = std::time(nullptr);
    std::tm utc {};
#if defined(_WIN32)
    gmtime_s(&utc, &now);
#else
    gmtime_r(&now, &utc);
#endif
    char stamp[32];
    std::snprintf(stamp, sizeof(stamp), "%02d:%02d:%02d", utc.tm_hour, utc.tm_min, utc.tm_sec);
    std::fprintf(stderr, "[ggmcp %s] %s\n", stamp, line.c_str());
    std::fflush(stderr);
}

std::shared_ptr<http::Router> makeRouter(const Options& options, Protocol& protocol,
                                        const Authorizer& authorizer, SessionStore& sessions,
                                        const ToolRegistry& tools) {
    auto router = std::make_shared<http::Router>();
    const std::string toolNames = describeTools(tools);
    const Options settings = options;

    const auto endpoint = settings.endpoint;
    const auto healthPath = settings.healthPath;
    const auto indexPath = settings.indexPath;
    const auto metadataPath = settings.metadataPath;

    router->add("POST", endpoint, [&protocol, &authorizer, settings](const http::Request& request) {
        if (!originAllowed(request, settings)) {
            return finish(request, settings, http::Router::errorBody(403, kCorsDenied));
        }
        const auto decision = authorizer.authorize(request);
        if (!decision.allowed) {
            http::Response response =
                http::Response::json(401, authorizer.resourceMetadata(request));
            response.setHeader("WWW-Authenticate", decision.wwwAuthenticate);
            return finish(request, settings, std::move(response));
        }

        Inbound inbound;
        inbound.transport = "http";
        inbound.body = request.body;
        inbound.sessionId = request.header(protocol::kSessionIdHeader);
        inbound.contentType = request.header("Content-Type");
        inbound.accept = request.header("Accept");
        inbound.origin = request.header("Origin");
        inbound.userAgent = request.header("User-Agent");
        inbound.clientAddress = request.remoteAddress;
        inbound.protocolVersionHeader = request.header(protocol::kProtocolVersionHeader);

        return finish(request, settings, fromOutbound(protocol.handle(inbound)));
    });

    router->add("DELETE", endpoint,
                [&protocol, &authorizer, settings](const http::Request& request) {
                    if (!originAllowed(request, settings)) {
                        return finish(request, settings,
                                      http::Router::errorBody(403, kCorsDenied));
                    }
                    const auto decision = authorizer.authorize(request);
                    if (!decision.allowed) {
                        http::Response response =
                            http::Response::json(401, authorizer.resourceMetadata(request));
                        response.setHeader("WWW-Authenticate", decision.wwwAuthenticate);
                        return finish(request, settings, std::move(response));
                    }
                    const std::string sessionId = request.header(protocol::kSessionIdHeader);
                    const int status = protocol.destroy(sessionId);
                    if (status == 200) {
                        return finish(
                            request, settings,
                            http::Response::json(200, Json::object({{"message", "session closed"},
                                                                    {"session", sessionId}})));
                    }
                    std::string code = status == 404 ? "session_not_found" : "bad_request";
                    if (settings.sessionPolicy == SessionPolicy::Stateless) {
                        code = "stateless";
                    }
                    return finish(request, settings,
                                  http::Response::json(
                                      status,
                                      protocol::transportFault(code, "no session to close")));
                });

    router->add("GET", healthPath,
                [&authorizer, &sessions, settings, toolNames](const http::Request& request) {
                    const auto decision = authorizer.authorize(request);
                    if (!decision.allowed) {
                        http::Response response =
                            http::Response::json(401, authorizer.resourceMetadata(request));
                        response.setHeader("WWW-Authenticate", decision.wwwAuthenticate);
                        return finish(request, settings, std::move(response));
                    }
                    Json health = Json::object({{"status", "ok"},
                                                {"version", settings.serverVersion},
                                                {"transport", "http"},
                                                {"uptimeSeconds", uptimeSeconds()},
                                                {"sessions", static_cast<long long>(sessions.size())},
                                                {"endpoint", settings.endpoint},
                                                {"policy", toString(settings.sessionPolicy)},
                                                {"root", settings.root()},
                                                {"token", authorizer.sourceDescription()},
                                                {"alias", settings.alias},
                                                {"tools", toolNames},
                                                {"cors", settings.cors}});
                    return finish(request, settings, http::Response::json(200, health));
                });

    router->add("GET", metadataPath, [&authorizer, settings](const http::Request& request) {
        return finish(request, settings,
                      http::Response::json(200, authorizer.resourceMetadata(request)));
    });

    router->add("GET", indexPath,
                [&authorizer, settings, toolNames](const http::Request& request) {
                    const auto decision = authorizer.authorize(request);
                    if (!decision.allowed) {
                        http::Response response =
                            http::Response::json(401, authorizer.resourceMetadata(request));
                        response.setHeader("WWW-Authenticate", decision.wwwAuthenticate);
                        return finish(request, settings, std::move(response));
                    }
                    std::ostringstream html;
                    html << "<!doctype html><html><head><meta charset=\"utf-8\"><title>"
                         << htmlEscape(settings.serverTitle) << "</title></head><body>"
                         << "<h1>" << htmlEscape(settings.serverTitle) << " "
                         << htmlEscape(settings.serverVersion) << "</h1>"
                         << "<p>Model Context Protocol server over streamable HTTP.</p>"
                         << "<h2>Endpoints</h2><ul>"
                         << "<li><code>POST " << htmlEscape(settings.endpoint)
                         << "</code> &mdash; JSON-RPC</li>"
                         << "<li><code>DELETE " << htmlEscape(settings.endpoint)
                         << "</code> &mdash; close a session</li>"
                         << "<li><code>GET " << htmlEscape(settings.healthPath)
                         << "</code> &mdash; health</li>"
                         << "<li><code>GET " << htmlEscape(settings.metadataPath)
                         << "</code> &mdash; protected resource metadata</li></ul>"
                         << "<h2>Tools</h2><p>alias: "
                         << (settings.alias.empty() ? "none" : htmlEscape(settings.alias))
                         << "</p><p>" << htmlEscape(toolNames) << "</p>"
                         << "<p>session policy: " << toString(settings.sessionPolicy) << "</p>"
                         << "</body></html>";
                    http::Response response = http::Response::empty(200);
                    response.body = html.str();
                    response.setHeader("Content-Type", "text/html; charset=utf-8");
                    return finish(request, settings, std::move(response));
                });

    const auto preflightHandler = [settings](const http::Request& request) {
        return preflight(request, settings);
    };
    router->add("OPTIONS", endpoint, preflightHandler);
    router->add("OPTIONS", healthPath, preflightHandler);
    router->add("OPTIONS", indexPath, preflightHandler);
    router->add("OPTIONS", metadataPath, preflightHandler);

    return router;
}

HttpServer::HttpServer(Options options)
    : options_(std::move(options)), authorizer_(options_) {
    registerBuiltins(tools_, options_);
    tools_.setAlias(options_.alias);
    protocol_ = std::make_unique<Protocol>(options_, tools_, sessions_);
}

HttpServer::~HttpServer() {
    stop();
}

bool HttpServer::start(std::string& error) {
    if (!authorizer_.prepare(error)) {
        return false;
    }
    buildRouter();
    server_ = backends::makeHttpServer();
    server_->setRouter(router_);
    const Options settings = options_;
    server_->setLogger([settings](const std::string& line) {
        if (!settings.verbose) {
            return;
        }
        logLine(settings, line);
    });
    if (!server_->bind(options_.host, options_.port, error)) {
        return false;
    }
    return true;
}

void HttpServer::buildRouter() {
    router_ = makeRouter(options_, *protocol_, authorizer_, sessions_, tools_);
}

void HttpServer::wait() {
    if (!server_) {
        return;
    }
    std::string error;
    if (!server_->listen(error)) {
        logLine(options_, "http transport stopped: " + error);
    }
}

void HttpServer::stop() {
    if (server_) {
        server_->stop();
    }
}

int HttpServer::port() const {
    return server_ ? server_->port() : options_.port;
}

std::string HttpServer::describe() const {
    std::ostringstream out;
    out << "ggmcp " << options_.serverVersion << " over streamable http, listening on http://"
        << options_.host << ":" << port() << options_.endpoint << "\n"
        << "  json backend:  " << options_.jsonBackend << "\n"
        << "  http backend:  " << options_.httpBackend << "\n"
        << "  session policy: " << toString(options_.sessionPolicy) << "\n"
        << "  root:          " << options_.root()
        << (options_.allowOutsideRoot ? " (not enforced, --allow-outside-root)" : "") << "\n"
        << "  authorization: " << authorizer_.sourceDescription() << "\n"
        << "  alias:         " << (options_.alias.empty() ? std::string("none") : options_.alias)
        << "\n"
        << "  tools:         " << describeTools(tools_);
    return out.str();
}

} // namespace gg::mcp
