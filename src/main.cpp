// ggmcp - a tiny Model Context Protocol server in dependency-free C++17.
//
// SPDX-License-Identifier: MIT
//
// main.cpp - command line and transport selection. Default transport is MCP
// streamable HTTP on 127.0.0.1:8080; --stdio switches to newline-delimited
// JSON-RPC on stdin/stdout.

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "ggmcp/http_server.hpp"
#include "ggmcp/log.hpp"
#include "ggmcp/mcp_http.hpp"
#include "ggmcp/protocol.hpp"
#include "ggmcp/tools.hpp"

namespace {

void print_usage(const char* argv0) {
    const std::string program = argv0 ? argv0 : "ggmcp";
    std::printf(
        "%s %s - a Model Context Protocol server in dependency-free C++17\n"
        "\n"
        "Usage:\n"
        "  %s [options]\n"
        "\n"
        "Transports:\n"
        "  --http [HOST:]PORT   serve MCP over streamable HTTP (default: 127.0.0.1:8080)\n"
        "  --stdio              serve newline-delimited JSON-RPC on stdin/stdout\n"
        "\n"
        "Options:\n"
        "  --host HOST            bind address (default 127.0.0.1)\n"
        "  --port PORT            port; 0 asks the OS for a free one (default 8080)\n"
        "  --endpoint PATH        MCP endpoint path (default /mcp)\n"
        "  --allow-origin ORIGIN  accept an extra Origin; repeatable\n"
        "  --allow-any-origin     accept any Origin (drops the DNS-rebinding guard)\n"
        "  --no-health            do not serve GET /health\n"
        "  --no-index             do not serve GET /\n"
        "  --session-policy MODE  lenient (default) | strict | stateless\n"
        "  --require-session      alias for --session-policy strict: reject requests\n"
        "                         that lack a known Mcp-Session-Id\n"
        "  --no-session           alias for --session-policy stateless: never issue\n"
        "                         a session id, so clients need no handshake state\n"
        "  -v, --verbose          log every request to stderr\n"
        "  --version              print the version and exit\n"
        "  -h, --help             this text\n"
        "\n"
        "Try it:\n"
        "  %s &\n"
        "  curl -s localhost:8080/health\n"
        "  curl -si localhost:8080/mcp -H 'Content-Type: application/json' \\\n"
        "    -d '{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"initialize\",\"params\":\n"
        "         {\"protocolVersion\":\"%s\",\"clientInfo\":{\"name\":\"curl\",\"version\":\"0\"},\n"
        "          \"capabilities\":{}}}'\n"
        "\n"
        "By default echoing the Mcp-Session-Id response header is optional; clients\n"
        "that cannot retain it are served against an implicit session.\n",
        ggmcp::kDefaultServerName, ggmcp::kDefaultServerVersion, program.c_str(), program.c_str(),
        ggmcp::latest_protocol_version().c_str());
}

struct Options {
    bool stdio = false;
    std::string host = "127.0.0.1";
    std::uint16_t port = 8080;
    ggmcp::HttpOptions http;
    ggmcp::SessionPolicy session_policy = ggmcp::SessionPolicy::Lenient;
    bool require_session = false;
    bool no_session = false;
    bool verbose = false;
    bool help = false;
    bool version = false;
    std::string error;
};

/// Accepts "8080" or "127.0.0.1:8080". Leaves `host` alone for a bare port.
bool parse_host_port(const std::string& text, std::string& host, std::uint16_t& port) {
    std::string host_text = host;
    std::string port_text = text;

    const std::size_t colon = text.rfind(':');
    if (colon != std::string::npos) {
        host_text = text.substr(0, colon);
        port_text = text.substr(colon + 1);
        if (host_text.empty()) host_text = "127.0.0.1";
    }

    char* end = nullptr;
    const long parsed = std::strtol(port_text.c_str(), &end, 10);
    if (!end || *end != '\0' || parsed < 0 || parsed > 65535) return false;
    host = host_text;
    port = static_cast<std::uint16_t>(parsed);
    return true;
}

/// Parses a port-only value, rejecting anything that smells like a host.
bool parse_port(const std::string& text, std::uint16_t& port) {
    if (text.find(':') != std::string::npos) return false;
    std::string host = "127.0.0.1";
    if (!parse_host_port(text, host, port)) return false;
    return true;
}

Options parse_args(int argc, char** argv) {
    Options options;

    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        const auto next_value = [&]() -> const char* {
            if (i + 1 >= argc) {
                options.error = arg + " needs a value";
                return nullptr;
            }
            return argv[++i];
        };

        if (arg == "--stdio") {
            options.stdio = true;
        } else if (arg == "--http") {
            // The value is optional: only take it when it is not another flag.
            if (i + 1 < argc && argv[i + 1][0] != '-') {
                const std::string value = next_value();
                if (!parse_host_port(value, options.host, options.port)) {
                    options.error = "--http expects PORT or HOST:PORT, got '" + value + "'";
                    return options;
                }
            }
        } else if (arg == "--host") {
            if (const char* v = next_value()) options.host = v;
        } else if (arg.rfind("--host=", 0) == 0) {
            options.host = arg.substr(7);
        } else if (arg == "--port") {
            if (const char* v = next_value()) {
                if (!parse_port(v, options.port)) {
                    options.error = "--port expects a bare number, got '" + std::string(v) + "'";
                    return options;
                }
            }
        } else if (arg.rfind("--port=", 0) == 0) {
            if (!parse_port(arg.substr(7), options.port)) {
                options.error = "--port expects a bare number, got '" + arg.substr(7) + "'";
                return options;
            }
        } else if (arg == "--endpoint") {
            if (const char* v = next_value()) options.http.endpoint = v;
        } else if (arg.rfind("--endpoint=", 0) == 0) {
            options.http.endpoint = arg.substr(13);
        } else if (arg == "--allow-origin") {
            if (const char* v = next_value()) options.http.allowed_origins.emplace_back(v);
        } else if (arg.rfind("--allow-origin=", 0) == 0) {
            options.http.allowed_origins.push_back(arg.substr(15));
        } else if (arg == "--allow-any-origin") {
            options.http.allow_any_origin = true;
        } else if (arg == "--no-health") {
            options.http.enable_health_endpoint = false;
        } else if (arg == "--no-index") {
            options.http.enable_index_endpoint = false;
        } else if (arg == "--require-session") {
            options.require_session = true;
        } else if (arg == "--no-session") {
            options.no_session = true;
        } else if (arg == "--session-policy") {
            if (const char* v = next_value()) {
                if (!ggmcp::parse_session_policy(v, options.session_policy)) {
                    options.error = "--session-policy expects lenient|strict|stateless, got '" +
                                    std::string(v) + "'";
                    return options;
                }
            }
        } else if (arg.rfind("--session-policy=", 0) == 0) {
            const std::string value = arg.substr(17);
            if (!ggmcp::parse_session_policy(value, options.session_policy)) {
                options.error = "--session-policy expects lenient|strict|stateless, got '" + value + "'";
                return options;
            }
        } else if (arg == "-v" || arg == "--verbose") {
            options.verbose = true;
        } else if (arg == "--version") {
            options.version = true;
        } else if (arg == "-h" || arg == "--help") {
            options.help = true;
        } else {
            options.error = "unknown argument: " + arg;
            return options;
        }

        if (!options.error.empty()) return options;
    }

    if (options.require_session && options.no_session) {
        options.error = "--require-session and --no-session are mutually exclusive";
        return options;
    }
    if (options.require_session) options.session_policy = ggmcp::SessionPolicy::Strict;
    if (options.no_session) options.session_policy = ggmcp::SessionPolicy::Stateless;

    if (options.http.endpoint.empty()) options.http.endpoint = "/mcp";
    if (options.http.endpoint.front() != '/') options.http.endpoint = "/" + options.http.endpoint;
    return options;
}

}  // namespace

int main(int argc, char** argv) {
    const Options options = parse_args(argc, argv);

    if (options.help) {
        print_usage(argv[0]);
        return 0;
    }
    if (options.version) {
        std::printf("%s %s (MCP protocol %s)\n", ggmcp::kDefaultServerName, ggmcp::kDefaultServerVersion,
                    ggmcp::latest_protocol_version().c_str());
        return 0;
    }
    if (!options.error.empty()) {
        std::fprintf(stderr, "%s: %s\n", ggmcp::kDefaultServerName, options.error.c_str());
        std::fprintf(stderr, "try '%s --help'\n", argv[0] ? argv[0] : "ggmcp");
        return 2;
    }

    ggmcp::set_verbose(options.verbose);

    ggmcp::Dispatcher dispatcher(ggmcp::kDefaultServerName, ggmcp::kDefaultServerVersion);
    dispatcher.session_policy(options.session_policy);
    ggmcp::register_demo_tools(dispatcher);

    if (options.stdio) return ggmcp::serve_stdio(dispatcher);

    ggmcp::install_signal_handlers();

    ggmcp::HttpServer server(options.host, options.port, nullptr);
    std::string error;
    if (!server.listen(error)) {
        ggmcp::log_line("could not start: " + error);
        return 1;
    }

    // The handler needs the bound port to validate Origin headers, so it is
    // installed after listen() rather than in the constructor.
    server.set_handler(ggmcp::make_http_handler(dispatcher, options.http, server.port()));

    std::string tools;
    for (const auto& name : dispatcher.tool_names()) {
        if (!tools.empty()) tools += ", ";
        tools += name;
    }
    ggmcp::log_line(std::string(ggmcp::kDefaultServerName) + " " + ggmcp::kDefaultServerVersion +
                    " ready - tools: " + tools + " - session policy: " +
                    ggmcp::to_string(options.session_policy));
    if (options.http.enable_health_endpoint) ggmcp::log_line("GET /health reports status");
    ggmcp::log_line("press Ctrl-C to stop");

    const int status = server.serve();
    ggmcp::log_line("shutting down");
    return status;
}
