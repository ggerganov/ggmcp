// ggmcp - a tiny Model Context Protocol server in dependency-free C++17.
//
// SPDX-License-Identifier: MIT

#include "ggmcp/mcp_http.hpp"

#include "ggmcp/log.hpp"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdlib>

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

Response method_not_allowed(const std::string& allowed) {
    Response response = Response::json(405, Value::make_object(Object{
                                                {"error", Value::make_string("method not allowed")},
                                                {"allow", Value::make_string(allowed)},
                                            }).dump());
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

        // --- non-MCP conveniences -----------------------------------------
        if (!is_endpoint && options.enable_health_endpoint && request.path == "/health" && method == "get") {
            Object status;
            status["status"] = Value::make_string("ok");
            status["server"] = Value::make_string(dispatcher.server_name());
            status["version"] = Value::make_string(dispatcher.server_version());
            status["transport"] = Value::make_string("streamable-http");
            status["endpoint"] = Value::make_string(endpoint);
            status["protocolVersion"] = Value::make_string(latest_protocol_version());

            Array versions;
            for (const auto& v : supported_protocol_versions()) versions.push_back(Value::make_string(v));
            status["supportedProtocolVersions"] = Value::make_array(std::move(versions));

            Array tools;
            for (const auto& name : dispatcher.tool_names()) tools.push_back(Value::make_string(name));
            status["tools"] = Value::make_array(std::move(tools));
            status["sessions"] = Value::make_int(static_cast<std::int64_t>(dispatcher.session_count()));
            status["sessionPolicy"] = Value::make_string(to_string(dispatcher.session_policy()));

            return Response::json(200, Value::make_object(std::move(status)).dump());
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
            return Response::text(200, std::move(banner));
        }

        if (!is_endpoint) {
            return Response::json(404, Value::make_object(Object{
                                           {"error", Value::make_string("not found")},
                                           {"path", Value::make_string(request.path)},
                                           {"endpoint", Value::make_string(endpoint)},
                                       })
                                       .dump());
        }

        // --- endpoint routing ---------------------------------------------
        const std::string allowed = "POST, GET, DELETE, OPTIONS";

        if (method == "options") {
            Response response = Response::empty(204);
            response.set("Allow", allowed);
            response.set("Accept", "application/json, text/event-stream");
            if (options.allow_any_origin) {
                response.set("Access-Control-Allow-Origin", request.header("origin"));
                response.set("Access-Control-Allow-Headers", "Content-Type, Mcp-Session-Id, MCP-Protocol-Version, Last-Event-ID, Authorization");
                response.set("Access-Control-Allow-Methods", allowed);
                response.set("Access-Control-Max-Age", "600");
            }
            return response;
        }

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
                return Response::json(404, Value::make_object(Object{
                                               {"error", Value::make_string("unknown session")},
                                           })
                                           .dump());
            }
            dispatcher.destroy_session(session_id);
            log_debug("session " + session_id + " closed by client");
            return Response::json(200, Value::make_object(Object{{"deleted", Value::make_bool(true)}}).dump());
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
                return Response::json(403, Value::make_object(Object{{"error", Value::make_string("Origin not allowed")},
                                                                      {"origin", Value::make_string(origin)}})
                                              .dump());
            }
        }

        // --- Accept / Content-Type negotiation ----------------------------
        const std::string accept = request.header("accept");
        if (!accept.empty() && !contains_any(accept, {"application/json", "*/*"})) {
            Response response = Response::json(406, Value::make_object(Object{
                                                        {"error", Value::make_string("this endpoint returns application/json")},
                                                        {"accept", Value::make_string(accept)},
                                                    })
                                                    .dump());
            response.set("Allow", allowed);
            return response;
        }

        const std::string content_type = request.header("content-type");
        if (!content_type.empty() && !contains_any(content_type, {"application/json"})) {
            return Response::json(415, Value::make_object(Object{
                                           {"error", Value::make_string("Content-Type must be application/json")},
                                           {"content-type", Value::make_string(content_type)},
                                       })
                                       .dump());
        }

        if (!protocol_header_ok(request)) {
            return Response::json(400, Value::make_object(Object{
                                           {"error", Value::make_string("Unsupported MCP-Protocol-Version")},
                                           {"received", Value::make_string(request.header("mcp-protocol-version"))},
                                       })
                                       .dump());
        }

        Context ctx;
        ctx.transport = "http";
        ctx.peer = request.peer;
        ctx.session_id = request.header("mcp-session-id");

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
