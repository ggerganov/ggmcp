#include "ggmcp/http.hpp"

#include <sys/socket.h>

#include <algorithm>
#include <cctype>
#include <cstring>
#include <sstream>
#include <string>

#include <httplib.h>

#include "ggmcp/auth.hpp"
#include "ggmcp/config.hpp"
#include "ggmcp/json.hpp"
#include "ggmcp/protocol.hpp"
#include "ggmcp/tools.hpp"
#include "ggmcp/version.hpp"

namespace ggmcp {
namespace {

constexpr const char* kMetadataPath = "/.well-known/oauth-protected-resource";
constexpr const char* kHealthPath = "/health";
constexpr const char* kSessionHeader = "Mcp-Session-Id";
constexpr const char* kVersionHeader = "MCP-Protocol-Version";

const char* status_message(int status) {
    switch (status) {
        case 204: return "no content";
        case 400: return "bad request";
        case 401: return "unauthorized";
        case 403: return "forbidden";
        case 404: return "not found";
        case 405: return "method not allowed";
        case 406: return "not acceptable";
        case 408: return "request timeout";
        case 413: return "payload too large";
        case 414: return "URI too long";
        case 415: return "unsupported media type";
        case 500: return "internal server error";
        default: return "error";
    }
}

std::string trimmed(std::string text) {
    while (!text.empty() && std::isspace(static_cast<unsigned char>(text.front()))) text.erase(text.begin());
    while (!text.empty() && std::isspace(static_cast<unsigned char>(text.back()))) text.pop_back();
    return text;
}

std::string lowered(std::string text) {
    for (char& c : text) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return text;
}

bool one_of(const std::vector<std::string>& list, const std::string& value) {
    for (const std::string& item : list) {
        if (item == "*" || item == value) return true;
    }
    return false;
}

// A missing Accept, `*/*` and anything that names JSON are all fine; an Accept
// that rules JSON out is a 406.
bool accept_allows_json(const std::string& accept) {
    const std::string header = lowered(trimmed(accept));
    if (header.empty()) return true;
    std::stringstream stream(header);
    std::string part;
    while (std::getline(stream, part, ',')) {
        const std::size_t semicolon = part.find(';');
        if (semicolon != std::string::npos) part = trimmed(part.substr(0, semicolon));
        if (part.empty()) continue;
        if (part == "*/*" || part == "application/json" || part == "application/*" ||
            part == "text/json" || part == "json") {
            return true;
        }
    }
    return false;
}

bool content_is_json(const std::string& content_type) {
    const std::string type = lowered(trimmed(content_type));
    if (type.empty()) return false;
    const std::size_t semicolon = type.find(';');
    const std::string head = trimmed(semicolon == std::string::npos ? type : type.substr(0, semicolon));
    return head == "application/json" || head == "text/json" || head == "application/json-seq";
}

std::string html_escape(const std::string& text) {
    std::string out;
    out.reserve(text.size());
    for (char c : text) {
        switch (c) {
            case '&': out += "&amp;"; break;
            case '<': out += "&lt;"; break;
            case '>': out += "&gt;"; break;
            case '"': out += "&quot;"; break;
            default: out += c; break;
        }
    }
    return out;
}

}  // namespace

struct HttpTransport::Impl {
    const Config* config = nullptr;
    Core* core = nullptr;
    const TokenStore* auth = nullptr;
    const ToolRegistry* tools = nullptr;
    httplib::Server server;

    enum class Route { Mcp, Health, Index, Metadata, None };

    Route route_of(const std::string& path) const {
        if (path == config->endpoint) return Route::Mcp;
        if (path == kHealthPath) return Route::Health;
        if (path == std::string(kMetadataPath)) return Route::Metadata;
        if (path == "/") return Route::Index;
        return Route::None;
    }

    std::string origin_of(const httplib::Request& req) const {
        std::string host = req.get_header_value("Host");
        if (host.empty()) host = config->host + ":" + std::to_string(config->port);
        return "http://" + host;
    }

    bool origin_allowed(const httplib::Request& req) const {
        const std::string origin = req.get_header_value("Origin");
        if (origin.empty()) return true;
        if (!config->strict_origin) return true;
        if (one_of(config->allow_origin, origin)) return true;
        return origin == origin_of(req);
    }

    // The origin is echoed rather than widened to `*`.
    void add_cors(const httplib::Request& req, httplib::Response& res) const {
        if (!config->cors) return;
        const std::string origin = req.get_header_value("Origin");
        if (origin.empty()) return;
        res.set_header("Access-Control-Allow-Origin", origin);
        res.set_header("Vary", "Origin");
        res.set_header("Access-Control-Expose-Headers",
                       std::string(kSessionHeader) + ", " + kVersionHeader);
    }

    void json(httplib::Response& res, int status, const Json& body) const {
        res.status = status;
        res.set_content(dump(body), "application/json");
    }

    void protocol_error(const httplib::Request& req, httplib::Response& res, int status,
                        const std::string& code, const std::string& message) const {
        add_cors(req, res);
        json(res, status, protocol_fault(code, message));
    }

    std::string allowed_for(Route route) const {
        if (route == Route::Mcp) return "POST, DELETE, OPTIONS";
        if (route == Route::None) return "GET, POST, DELETE, OPTIONS";
        return "GET, OPTIONS";
    }

    // A refused token answers with the RFC 9728 document and the challenge. The
    // preflight never gets here: it carries no token by definition.
    bool authorize(const httplib::Request& req, httplib::Response& res) const {
        const bool query_has_token = req.has_param("token") || req.has_param("access_token");
        const TokenStore::Verdict verdict =
            auth->check(req.get_header_value("Authorization"), query_has_token);
        if (verdict == TokenStore::Verdict::Ok) return true;

        std::string reason = "this server is a protected resource: send a bearer token";
        if (verdict == TokenStore::Verdict::InQuery) {
            reason = "a bearer token does not belong in the query string: send the Authorization header";
        } else if (verdict == TokenStore::Verdict::Wrong) {
            reason = "that is not the bearer token this server serves";
        }

        add_cors(req, res);
        res.set_header("WWW-Authenticate", auth->challenge(origin_of(req), reason));
        json(res, 401, auth->metadata(origin_of(req), config->endpoint));
        return false;
    }

    // A refused origin is a 403 with no CORS headers, so the browser reads nothing.
    bool gate_origin(const httplib::Request& req, httplib::Response& res) const {
        if (origin_allowed(req)) return true;
        res.status = 403;
        res.set_content(
            dump(routing_fault(403,
                               "origin '" + req.get_header_value("Origin") + "' is not allowed")),
            "application/json");
        return false;
    }

    RequestEnvelope envelope_of(const httplib::Request& req) const {
        RequestEnvelope env;
        env.transport = "http";
        env.address = req.remote_addr + ":" + std::to_string(req.remote_port);
        env.session_id = req.get_header_value(kSessionHeader);
        env.protocol_version = req.get_header_value(kVersionHeader);
        return env;
    }

    void publish(const httplib::Request& req, httplib::Response& res, const Outcome& outcome) const {
        add_cors(req, res);
        res.status = outcome.status;
        if (outcome.has_body) {
            res.set_content(dump_safely(outcome.body), "application/json");
        } else {
            res.set_header("Content-Type", "application/json");
        }
        if (!outcome.session_id.empty()) res.set_header(kSessionHeader, outcome.session_id);
    }

    void on_mcp(const httplib::Request& req, httplib::Response& res) const {
        if (!gate_origin(req, res)) return;
        if (!authorize(req, res)) return;

        if (req.method == "DELETE") {
            publish(req, res, core->close_session(req.get_header_value(kSessionHeader)));
            return;
        }

        if (!accept_allows_json(req.get_header_value("Accept"))) {
            protocol_error(req, res, 406, "not_acceptable",
                           "this server answers in JSON; send an Accept that allows it");
            return;
        }
        if (!content_is_json(req.get_header_value("Content-Type"))) {
            protocol_error(req, res, 415, "unsupported_media_type",
                           "the request body must be application/json");
            return;
        }
        if (req.body.size() > config->max_body) {
            res.status = 413;
            res.set_header("Content-Type", "application/json");
            return;
        }

        const RequestEnvelope env = envelope_of(req);
        publish(req, res, core->handle_body(req.body, env));
    }

    void on_preflight(const httplib::Request& req, httplib::Response& res) const {
        const std::string requested = lowered(req.get_header_value("Access-Control-Request-Method"));
        const std::string route_allowed = allowed_for(route_of(req.path));

        // A verb this server does not register is refused now, rather than
        // surprising the browser on the request that would have followed.
        std::stringstream want(requested);
        std::string verb;
        while (std::getline(want, verb, ',')) {
            const std::string folded = lowered(trimmed(verb));
            if (folded.empty()) continue;
            if (lowered(route_allowed).find(folded) == std::string::npos) {
                res.status = 405;
                res.set_header("Allow", route_allowed);
                res.set_content(dump(routing_fault(405, status_message(405))), "application/json");
                return;
            }
        }

        // The answer names every method this path serves, not just the one asked for.
        const std::string methods = route_allowed;

        std::string headers = trimmed(req.get_header_value("Access-Control-Request-Headers"));
        if (headers.empty()) {
            headers = std::string("Content-Type, Accept, Authorization, ") + kSessionHeader +
                      ", " + kVersionHeader + ", Last-Event-ID";
        }

        add_cors(req, res);
        res.set_header("Access-Control-Allow-Methods", methods);
        res.set_header("Access-Control-Allow-Headers", headers);
        res.set_header("Access-Control-Max-Age", "600");
        res.status = 204;
        res.set_header("Allow", route_allowed);
    }

    void on_health(const httplib::Request& req, httplib::Response& res) const {
        add_cors(req, res);
        json(res, 200, core->health());
    }

    void on_metadata(const httplib::Request& req, httplib::Response& res) const {
        add_cors(req, res);
        json(res, 200, auth->metadata(origin_of(req), config->endpoint));
    }

    void on_index(const httplib::Request& req, httplib::Response& res) const {
        const std::string origin = origin_of(req);
        const std::string url = origin + config->endpoint;

        std::ostringstream page;
        page << "<!doctype html>\n<html lang=\"en\">\n<head>\n<meta charset=\"utf-8\">\n"
             << "<title>" << html_escape(config->server_title()) << "</title>\n</head>\n<body>\n"
             << "<h1>" << html_escape(config->server_title()) << "</h1>\n"
             << "<p>Model Context Protocol server " << html_escape(kVersion) << ", speaking "
             << kProtocolVersions[0] << ", " << kProtocolVersions[1] << " and "
             << kProtocolVersions[2] << ".</p>\n"
             << "<h2>Endpoint</h2>\n<ul>\n"
             << "<li><code>POST " << html_escape(config->endpoint) << "</code> &mdash; JSON-RPC, "
             << "at <code>" << html_escape(url) << "</code></li>\n"
             << "<li><code>DELETE " << html_escape(config->endpoint)
             << "</code> &mdash; close the session named by <code>" << kSessionHeader
             << "</code></li>\n"
             << "<li><code>GET " << kHealthPath << "</code> &mdash; what this instance is</li>\n"
             << "<li><code>GET " << kMetadataPath << "</code> &mdash; authorization metadata</li>\n"
             << "</ul>\n<h2>Session policy</h2>\n<p>"
             << to_string(config->session_policy) << "</p>\n<h2>Root</h2>\n<p><code>"
             << html_escape(config->fs_root) << "</code>, "
             << (config->confined() ? "confined" : "not confined") << "</p>\n<h2>Authorization</h2>\n<p>"
             << (config->require_token ? "bearer token" : "off") << "</p>\n<h2>Tools</h2>\n<ul>\n";
        const Json published = tools->published();
        for (const Json& tool : published) {
            page << "<li><code>" << html_escape(tool["name"].get<std::string>()) << "</code> &mdash; "
                 << html_escape(tool["description"].get<std::string>()) << "</li>\n";
        }
        page << "</ul>\n</body>\n</html>\n";

        res.set_header("Content-Type", "text/html; charset=utf-8");
        res.set_content(page.str(), "text/html; charset=utf-8");
    }

    httplib::Server::HandlerResponse pre_route(const httplib::Request& req,
                                               httplib::Response& res) const {
        const Route route = route_of(req.path);
        if (route == Route::None) {
            res.status = 404;
            add_cors(req, res);
            res.set_header("Allow", allowed_for(route));
            res.set_content(dump(routing_fault(404, "no such path: " + req.path)),
                            "application/json");
            return httplib::Server::HandlerResponse::Handled;
        }

        if (!gate_origin(req, res)) {
            return httplib::Server::HandlerResponse::Handled;
        }

        if (req.method == "OPTIONS") {
            if (!config->cors) {
                res.status = 405;
                res.set_header("Allow", allowed_for(route));
                res.set_content(dump(routing_fault(405, status_message(405))), "application/json");
                return httplib::Server::HandlerResponse::Handled;
            }
            on_preflight(req, res);
            return httplib::Server::HandlerResponse::Handled;
        }

        if (route == Route::Metadata) {
            on_metadata(req, res);
            return httplib::Server::HandlerResponse::Handled;
        }

        if (route != Route::Mcp) {
            if (!authorize(req, res)) {
                return httplib::Server::HandlerResponse::Handled;
            }
            if (req.method == "GET") {
                if (route == Route::Index) {
                    on_index(req, res);
                } else {
                    on_health(req, res);
                }
            } else {
                res.status = 405;
                res.set_header("Allow", allowed_for(route));
                res.set_content(dump(routing_fault(405, status_message(405))), "application/json");
            }
            return httplib::Server::HandlerResponse::Handled;
        }

        // The endpoint: POST and DELETE need the body the library is about to read.
        if (req.method == "POST" || req.method == "DELETE") {
            return httplib::Server::HandlerResponse::Unhandled;
        }

        if (!authorize(req, res)) {
            return httplib::Server::HandlerResponse::Handled;
        }
        res.status = 405;
        res.set_header("Allow", allowed_for(route));
        res.set_content(dump(routing_fault(405, status_message(405))), "application/json");
        return httplib::Server::HandlerResponse::Handled;
    }
};

HttpTransport::HttpTransport(const Config& config, Core& core, const TokenStore& auth,
                             const ToolRegistry& tools)
    : config_(&config), core_(&core), auth_(&auth), tools_(&tools) {
    impl_ = std::unique_ptr<Impl>(new Impl());
    impl_->config = config_;
    impl_->core = core_;
    impl_->auth = auth_;
    impl_->tools = tools_;

    Impl* impl = impl_.get();

    // SO_REUSEADDR alone: a restart over a socket in TIME_WAIT must work, and a
    // port that is already serving must be refused rather than shared.
    impl->server.set_socket_options([](int sock) {
        const int one = 1;
        ::setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const void*>(&one),
                     sizeof(one));
    });
    impl->server.set_keep_alive_max_count(64);
    impl->server.set_keep_alive_timeout(30);
    impl->server.set_payload_max_length(
        (std::max<std::size_t>)(config.max_body * 8, 16u * 1024u * 1024u));

    impl->server.set_pre_routing_handler(
        [impl](const httplib::Request& req, httplib::Response& res) {
            return impl->pre_route(req, res);
        });
    impl->server.set_error_handler([](const httplib::Request& req, httplib::Response& res) {
        (void)req;
        // A fault this server raised already carries its body. What is left is a
        // fault the library raised on its own, and that still deserves JSON.
        if (res.body.empty() && res.status >= 400 && res.status != 202 && res.status != 204 &&
            res.status != 413) {
            res.set_content(dump(routing_fault(res.status, status_message(res.status))),
                            "application/json");
        }
        return true;
    });
    impl->server.set_exception_handler([impl](const httplib::Request& req,
                                              httplib::Response& res, std::exception_ptr) {
        impl->protocol_error(req, res, 500, "internal_error", "this request did not complete");
    });

    impl->server.Post(config.endpoint,
                      [impl](const httplib::Request& req, httplib::Response& res) {
                          impl->on_mcp(req, res);
                      });
    impl->server.Delete(config.endpoint,
                        [impl](const httplib::Request& req, httplib::Response& res) {
                            impl->on_mcp(req, res);
                        });
}

HttpTransport::~HttpTransport() = default;

bool HttpTransport::bind(std::string& error) {
    if (!impl_->server.bind_to_port(config_->host, config_->port)) {
        error = "cannot bind " + config_->host + ":" + std::to_string(config_->port) + ": " +
                std::strerror(errno);
        return false;
    }
    bound_ = true;
    return true;
}

void HttpTransport::run() { impl_->server.listen_after_bind(); }

void HttpTransport::stop() { impl_->server.stop(); }

}  // namespace ggmcp
