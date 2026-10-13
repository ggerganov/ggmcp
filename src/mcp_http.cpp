// ggmcp - a tiny Model Context Protocol server built on vendored single-header libraries.
//
// SPDX-License-Identifier: MIT

#include "ggmcp/mcp_http.hpp"

#include "ggmcp/log.hpp"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <initializer_list>
#include <string>
#include <utility>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>

namespace ggmcp {
namespace {

std::string lower(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

bool contains_any(const std::string& haystack, const std::vector<std::string>& needles) {
    const std::string h = lower(haystack);
    for (const auto& needle : needles) {
        if (h.find(lower(needle)) != std::string::npos) return true;
    }
    return false;
}

/// MCP-Protocol-Version on a post-handshake request must be one we speak.
bool protocol_header_ok(const Request& request) {
    const std::string version = request.header("mcp-protocol-version");
    if (version.empty()) return true;   // older clients omit it; treat as pre-2025-06-18
    const auto& supported = supported_protocol_versions();
    return std::find(supported.begin(), supported.end(), version) != supported.end();
}

/// True for "localhost", anything in 127.0.0.0/8, and ::1. Parsing the
/// address rather than prefix-matching the text is what stops
/// "http://127.0.0.1.evil.example" from passing for loopback.
bool host_is_loopback(const std::string& host) {
    if (host == "localhost") return true;

    ::in_addr ipv4{};
    if (::inet_pton(AF_INET, host.c_str(), &ipv4) == 1) {
        return (ntohl(ipv4.s_addr) >> 24) == 127;
    }

    ::in6_addr ipv6{};
    if (::inet_pton(AF_INET6, host.c_str(), &ipv6) == 1) {
        return IN6_IS_ADDR_LOOPBACK(&ipv6) != 0;
    }

    return false;
}

/// Maps a dispatcher Reply onto an HTTP Response.
/// A notification must produce a bare 202 with no body at all.
Response json_rpc_response(Reply reply) {
    if (!reply.has_body) return Response::empty(reply.status);
    Response response = Response::json(reply.status, std::move(reply.body));
    if (!reply.session_id.empty()) response.set("Mcp-Session-Id", reply.session_id);
    return response;
}

/// The body of a transport-level refusal: `fields` in the order written, so
/// the JSON reads the way the sentence does.
Response error_response(int status, std::initializer_list<std::pair<std::string, std::string>> fields) {
    Object body;
    for (const auto& field : fields) body[field.first] = Value(field.second);
    return Response::json(status, dump(Value(std::move(body))));
}

Response method_not_allowed(const std::string& allowed) {
    Response response = error_response(405, {{"error", "method not allowed"}, {"allow", allowed}});
    response.set("Allow", allowed);
    return response;
}

}  // namespace

bool origin_is_loopback(const std::string& origin, std::uint16_t bound_port) {
    // Expected shape: scheme://host[:port]
    const std::string scheme_sep = "://";
    const std::size_t sep = origin.find(scheme_sep);
    if (sep == std::string::npos) return false;
    const std::string scheme = lower(origin.substr(0, sep));
    if (scheme != "http" && scheme != "https") return false;

    std::string rest = origin.substr(sep + scheme_sep.size());
    const std::size_t slash = rest.find('/');
    if (slash != std::string::npos) rest = rest.substr(0, slash);
    const std::size_t colon = rest.rfind(':');
    std::string host = colon == std::string::npos ? rest : rest.substr(0, colon);
    const std::string port = colon == std::string::npos ? std::string() : rest.substr(colon + 1);

    host = lower(host);
    if (host.size() >= 2 && host.front() == '[' && host.back() == ']') {
        host = host.substr(1, host.size() - 2);   // IPv6 literals arrive bracketed
    }
    if (!host_is_loopback(host)) return false;

    if (port.empty()) return true;   // scheme default port
    char* end = nullptr;
    const unsigned long parsed = std::strtoul(port.c_str(), &end, 10);
    return end && *end == '\0' && static_cast<std::uint16_t>(parsed) == bound_port;
}

/// Absolute URL to advertise for the metadata document.
///
/// Prefers an explicit operator override, then the Host header (so a deployment
/// behind a reverse proxy advertises the name the proxy terminates), then the
/// bound address. A Host header that does not look like a bare authority is
/// ignored rather than echoed: this string is handed to clients inside a header
/// they may follow.
std::string advertised_base(const std::string& public_url, const std::string& host, std::uint16_t bound_port) {
    if (!public_url.empty()) return public_url;

    const bool looks_like_authority =
        !host.empty() && host.find_first_of(" /@?#\\\r\n") == std::string::npos;
    if (looks_like_authority) return "http://" + host;

    return "http://127.0.0.1:" + std::to_string(bound_port);
}

HttpServer::Handler make_http_handler(Dispatcher& dispatcher, HttpOptions options, std::uint16_t bound_port) {
    if (options.allowed_origins.empty()) {
        // Nothing configured: accept the loopback names browsers and local
        // tooling use on the port we actually bound.
        options.allowed_origins.push_back("http://localhost:" + std::to_string(bound_port));
        options.allowed_origins.push_back("http://127.0.0.1:" + std::to_string(bound_port));
        options.allowed_origins.push_back("http://[::1]:" + std::to_string(bound_port));
    }

    return [&dispatcher, options = std::move(options), bound_port](const Request& request) {
        const std::string endpoint = options.endpoint.empty() ? std::string("/mcp") : options.endpoint;
        const std::string method = lower(request.method);
        const bool is_endpoint = request.path == endpoint || request.path == endpoint + "/";
        const std::shared_ptr<TokenStore> tokens = options.tokens;
        const bool auth_required = tokens && tokens->required();

        Identity identity;

        // --- routes that an unauthenticated caller may always reach ---------
        //
        // Two, and only two. The metadata document exists precisely to be fetched
        // before you have a credential. The CORS preflight is exempt because a
        // browser does not send Authorization on a preflight: answering 401 there
        // makes the server unreachable from any web page rather than merely
        // unreadable.
        if (tokens && !is_endpoint && method == "get" && request.path == tokens->metadata_path()) {
            Response response = Response::json(200, tokens->metadata_json(
                                                        advertised_base(tokens->public_url(), request.header("host"),
                                                                        bound_port) +
                                                        endpoint));
            response.set("Cache-Control", "no-store");
            response.set("X-Content-Type-Options", "nosniff");
            return response;
        }

        if (is_endpoint && method == "options") {
            Response response = Response::empty(204);
            response.set("Allow", "POST, GET, DELETE, OPTIONS");
            response.set("Accept", "application/json, text/event-stream");
            if (options.allow_any_origin) {
                response.set("Access-Control-Allow-Origin", request.header("origin"));
                response.set("Access-Control-Allow-Headers",
                             "Content-Type, Mcp-Session-Id, MCP-Protocol-Version, Last-Event-ID, Authorization");
                response.set("Access-Control-Allow-Methods", "POST, GET, DELETE, OPTIONS");
                response.set("Access-Control-Max-Age", "600");
            }
            return response;
        }

        // --- everything else needs the credential when one is required -------
        if (tokens) {
            const AuthResult auth = tokens->authorize(request);
            if (!auth.ok) {
                const std::string metadata_url =
                    advertised_base(tokens->public_url(), request.header("host"), bound_port) +
                    tokens->metadata_path();
                log_debug("401 " + method + " " + request.path + ": " + auth.description);

                Response response = error_response(
                    401, {{"error", "unauthorized"},
                          {"error_description", auth.description},
                          {"resource_metadata", metadata_url}});
                response.set("WWW-Authenticate", tokens->www_authenticate(metadata_url, auth.oauth_error,
                                                                         auth.description));
                response.set("Cache-Control", "no-store");
                response.set("X-Content-Type-Options", "nosniff");
                return response;
            }
            identity = auth.identity;
        }

        // --- non-MCP conveniences -----------------------------------------
        if (!is_endpoint && options.enable_health_endpoint && request.path == "/health" && method == "get") {
            Object status;
            status["status"] = Value("ok");
            status["server"] = Value(dispatcher.server_name());
            status["version"] = Value(dispatcher.server_version());
            status["transport"] = Value("streamable-http");
            status["endpoint"] = Value(endpoint);
            status["protocolVersion"] = Value(latest_protocol_version());

            Array versions;
            for (const auto& v : supported_protocol_versions()) versions.push_back(Value(v));
            status["supportedProtocolVersions"] = Value(std::move(versions));

            Array tools;
            for (const auto& name : dispatcher.tool_names()) tools.push_back(Value(name));
            status["tools"] = Value(std::move(tools));
            status["sessions"] = Value(static_cast<std::int64_t>(dispatcher.session_count()));
            status["sessionPolicy"] = Value(to_string(dispatcher.session_policy()));
            // Which tools exist is configuration, and configuration is worth
            // protecting: with auth on, /health is behind the token too.
            status["authorization"] = Value(auth_required ? "bearer" : "anonymous");

            return Response::json(200, dump(Value(std::move(status))));
        }

        if (!is_endpoint && options.enable_index_endpoint && request.path == "/" && method == "get") {
            std::string banner;
            banner += dispatcher.server_name() + " " + dispatcher.server_version() +
                      " - MCP server over streamable HTTP\n\n";
            banner += "This endpoint speaks JSON-RPC 2.0, not REST. POST a message to " + endpoint + ".\n\n";
            banner += "  curl -s " + endpoint + " -H 'Content-Type: application/json' -d \\\n";
            banner += "    '{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"initialize\",\"params\":{\"protocolVersion\":\"" +
                      latest_protocol_version() + "\",\"clientInfo\":{\"name\":\"curl\",\"version\":\"0\"},\"capabilities\":{}}}'\n\n";
            banner += "  curl -si " + endpoint + " -H 'Content-Type: application/json' \\\n";
            banner += "    -H 'Mcp-Session-Id: <id>' -H 'MCP-Protocol-Version: " + latest_protocol_version() +
                      "' -d \\\n";
            banner += "    '{\"jsonrpc\":\"2.0\",\"id\":2,\"method\":\"tools/list\"}'\n\n";
            banner += "Session policy: " + std::string(to_string(dispatcher.session_policy())) + ".\n";
            switch (dispatcher.session_policy()) {
                case SessionPolicy::Stateless:
                    banner += "This server issues no Mcp-Session-Id; every POST stands on its own.\n";
                    break;
                case SessionPolicy::Strict:
                    banner += "Requests without a known Mcp-Session-Id are rejected (-32002).\n";
                    break;
                case SessionPolicy::Lenient:
                    banner += "Echoing Mcp-Session-Id is optional here: requests without one are\n";
                    banner += "served against an implicit session.\n";
                    break;
            }
            if (auth_required) {
                banner += "\nAuthorization is required: send 'Authorization: Bearer <token>' on every\n";
                banner += "request, including this one. A missing or unusable token gets 401 with a\n";
                banner += "WWW-Authenticate header pointing at " + tokens->metadata_path() + ".\n";
            }
            return Response::text(200, std::move(banner));
        }

        if (!is_endpoint) {
            return error_response(404, {{"error", "not found"},
                                         {"path", request.path},
                                         {"endpoint", endpoint}});
        }

        // --- endpoint routing ---------------------------------------------
        const std::string allowed = "POST, GET, DELETE, OPTIONS";

        if (method == "get") {
            // The spec permits 405 here for servers that do not push
            // server-originated messages over SSE. We do not.
            return method_not_allowed("POST, DELETE, OPTIONS");
        }

        if (method == "delete") {
            if (dispatcher.session_policy() == SessionPolicy::Stateless) {
                // Nothing to tear down, and a 404 would imply a session existed.
                log_debug("DELETE ignored: server is stateless");
                return method_not_allowed("POST, GET, OPTIONS");
            }
            const std::string session_id = request.header("mcp-session-id");
            if (session_id.empty() || !dispatcher.session_exists(session_id)) {
                return error_response(404, {{"error", "unknown session"}});
            }
            dispatcher.destroy_session(session_id);
            log_debug("session " + session_id + " closed by client");
            return Response::json(200, dump(Value(Object{{"deleted", Value(true)}})));
        }

        if (method != "post") return method_not_allowed(allowed);

        // --- Origin (DNS rebinding protection) ----------------------------
        const std::string origin = request.header("origin");
        if (!options.disable_origin_check && !origin.empty()) {
            const auto allowed_origin = std::find_if(options.allowed_origins.begin(), options.allowed_origins.end(),
                                                     [&](const std::string& o) { return lower(o) == lower(origin); });
            if (allowed_origin == options.allowed_origins.end() &&
                !(options.allow_any_origin || origin_is_loopback(origin, bound_port))) {
                log_line("rejected Origin: " + origin);
                return error_response(403, {{"error", "Origin not allowed"}, {"origin", origin}});
            }
        }

        // --- Accept / Content-Type negotiation ----------------------------
        const std::string accept = request.header("accept");
        if (!accept.empty() && !contains_any(accept, {"application/json", "*/*"})) {
            Response response = error_response(
                406, {{"error", "this endpoint returns application/json"}, {"accept", accept}});
            response.set("Allow", allowed);
            return response;
        }

        const std::string content_type = request.header("content-type");
        if (!content_type.empty() && !contains_any(content_type, {"application/json"})) {
            return error_response(415, {{"error", "Content-Type must be application/json"},
                                         {"content-type", content_type}});
        }

        if (!protocol_header_ok(request)) {
            return error_response(400, {{"error", "Unsupported MCP-Protocol-Version"},
                                         {"received", request.header("mcp-protocol-version")}});
        }

        Context ctx;
        ctx.transport = "http";
        ctx.peer = request.peer;
        ctx.session_id = request.header("mcp-session-id");
        // Which credential opened this call, never the credential itself.
        ctx.auth = identity.describe();

        Reply reply = dispatcher.handle(request.body, std::move(ctx));

        Response response = json_rpc_response(std::move(reply));
        if (options.allow_any_origin && !origin.empty()) response.set("Access-Control-Allow-Origin", origin);
        response.set("Cache-Control", "no-store");
        return response;
    };
}

HttpServer::Handler make_http_handler(Dispatcher& dispatcher, std::uint16_t bound_port) {
    return make_http_handler(dispatcher, HttpOptions{}, bound_port);
}

}  // namespace ggmcp
