// ggmcp - a tiny Model Context Protocol server built on vendored single-header libraries.
//
// SPDX-License-Identifier: MIT
//
// main.cpp - command line and transport selection. The defaults are a server
// you can just run: MCP streamable HTTP on 0.0.0.0:8081, read/write/edit
// confined to your home directory, the shell tool on, Origin unchecked. Every
// one of those has a --no-* or --strict-* counterpart, and --stdio switches to
// newline-delimited JSON-RPC on stdin/stdout.

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

#include <pwd.h>
#include <unistd.h>

#include "ggmcp/http_server.hpp"
#include "ggmcp/log.hpp"
#include "ggmcp/mcp_http.hpp"
#include "ggmcp/protocol.hpp"
#include "ggmcp/tools.hpp"

namespace {

void print_usage(const char* argv0) {
    const std::string program = argv0 ? argv0 : "ggmcp";
    std::printf(
        "%s %s - a Model Context Protocol server built on vendored single-header libraries\n"
        "\n"
        "Usage:\n"
        "  %s [options]\n"
        "\n"
        "With no options at all, this is what you are running:\n"
        "\n"
        "  %s --host 0.0.0.0 --port 8081 --allow-bash --fs-root ~ --allow-any-origin\n"
        "\n"
        "Every interface, the home directory as the file root, a shell, and no Origin\n"
        "check - a server you do not have to configure. Take any of it back with:\n"
        "\n"
        "  %s --host 127.0.0.1 --no-file-tools --no-bash --strict-origin\n"
        "\n"
        "Transports:\n"
        "  --http [HOST:]PORT   serve MCP over streamable HTTP (default: 0.0.0.0:8081)\n"
        "  --stdio              serve newline-delimited JSON-RPC on stdin/stdout\n"
        "\n"
        "Options:\n"
        "  --host HOST            bind address (default 0.0.0.0: every interface)\n"
        "  --port PORT            port; 0 asks the OS for a free one (default 8081)\n"
        "  --endpoint PATH        MCP endpoint path (default /mcp)\n"
        "  --allow-origin ORIGIN  accept an extra Origin; repeatable. Naming one turns\n"
        "                         the default any-Origin off, so the list is what is allowed\n"
        "  --allow-any-origin     accept any Origin (default: on; drops the DNS-rebinding\n"
        "                         guard, so a browser page can drive this server)\n"
        "  --strict-origin        check Origin against the bound port instead: the guard\n"
        "                         back on, plus whatever --allow-origin named\n"
        "  --no-health            do not serve GET /health\n"
        "  --no-index             do not serve GET /\n"
        "  --session-policy MODE  lenient (default) | strict | stateless\n"
        "  --require-session      alias for --session-policy strict: reject requests\n"
        "                         that lack a known Mcp-Session-Id\n"
        "  --no-session           alias for --session-policy stateless: never issue\n"
        "                         a session id, so clients need no handshake state\n"
        "  --fs-root DIR          confine read/write/edit to DIR (default: your home\n"
        "                         directory; it must already exist)\n"
        "  --no-file-tools        do not offer read/write/edit at all\n"
        "  --allow-bash           offer the bash tool (default: on)\n"
        "  --no-bash              do not offer the bash tool\n"
        "  --bash-timeout SEC     wall-clock budget per bash command (default 30)\n"
        "  --require-token        require 'Authorization: Bearer <token>' on every request\n"
        "  --token-env NAME       take a token from environment variable NAME; repeatable\n"
        "                         (with no --token-* flag at all, $GGMCP_TOKEN is used)\n"
        "  --token-file PATH      take tokens from PATH, one per line; repeatable. Re-read\n"
        "                         when the file changes, so tokens rotate without a restart\n"
        "  --token-stdin          take one token from stdin at startup\n"
        "                         (there is no --token FLAG on purpose: argv is world-readable)\n"
        "  -v, --verbose          log every request to stderr\n"
        "  --version              print the version and exit\n"
        "  -h, --help             this text\n"
        "\n"
        "Try it:\n"
        "  %s &\n"
        "  curl -s localhost:8081/health\n"
        "  curl -si localhost:8081/mcp -H 'Content-Type: application/json' \\\n"
        "    -d '{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"initialize\",\"params\":\n"
        "         {\"protocolVersion\":\"%s\",\"clientInfo\":{\"name\":\"curl\",\"version\":\"0\"},\n"
        "          \"capabilities\":{}}}'\n"
        "\n"
        "By default echoing the Mcp-Session-Id response header is optional; clients\n"
        "that cannot retain it are served against an implicit session.\n"
        "\n"
        "read/write/edit are confined to --fs-root and cannot leave it, so they reach\n"
        "only files you already own. Nothing confines bash: those commands run with\n"
        "this process' privileges, and the default bind is every interface - which\n"
        "makes a bare invocation a shell for anyone who can reach the port. The\n"
        "defaults are for a machine you trust; --host 127.0.0.1, --no-bash and\n"
        "--no-file-tools are the three ways to ask for less than that.\n"
        "\n"
        "--require-token turns this into a bearer-token resource server: a missing or\n"
        "unusable token gets 401 with a WWW-Authenticate header pointing at\n"
        "/.well-known/oauth-protected-resource. It validates tokens, it does not\n"
        "issue them, and a token sent over plain HTTP is a postcard - put TLS in\n"
        "front before this listens on anything but 127.0.0.1.\n",
        ggmcp::kDefaultServerName, ggmcp::kDefaultServerVersion, program.c_str(), program.c_str(),
        program.c_str(), program.c_str(), ggmcp::latest_protocol_version().c_str());
}

struct Options {
    bool stdio = false;
    std::string host = "0.0.0.0";
    std::uint16_t port = 8081;
    ggmcp::HttpOptions http;
    ggmcp::SessionPolicy session_policy = ggmcp::SessionPolicy::Lenient;
    bool require_session = false;
    bool no_session = false;
    std::string fs_root;         ///< where read/write/edit are confined; empty = no file tools
    bool fs_root_given = false;  ///< --fs-root was typed, so its errors are the user's to see
    bool no_file_tools = false;  ///< --no-file-tools: do not resolve a default root either
    bool allow_bash = true;
    int bash_timeout = 30;
    bool require_token = false;
    bool token_stdin = false;
    ggmcp::AuthOptions auth;
    bool verbose = false;
    bool help = false;
    bool version = false;
    std::string error;
};

/// Accepts "8081" or "127.0.0.1:8081". Leaves `host` alone for a bare port.
bool parse_host_port(const std::string& text, std::string& host, std::uint16_t& port) {
    std::string host_text = host;
    std::string port_text = text;

    const std::size_t colon = text.rfind(':');
    if (colon != std::string::npos) {
        host_text = text.substr(0, colon);
        port_text = text.substr(colon + 1);
        // An empty host means the default one, not loopback: "--http :8081" and
        // "--http 8081" have to bind the same address.
        if (host_text.empty()) host_text = "0.0.0.0";
    }

    char* end = nullptr;
    const long parsed = std::strtol(port_text.c_str(), &end, 10);
    if (!end || *end != '\0' || parsed < 0 || parsed > 65535) return false;
    host = host_text;
    port = static_cast<std::uint16_t>(parsed);
    return true;
}

/// Parses a positive integer, rejecting anything else.
bool parse_int(const std::string& text, int& out) {
    if (text.empty()) return false;
    char* end = nullptr;
    const long parsed = std::strtol(text.c_str(), &end, 10);
    if (!end || *end != '\0' || parsed < 0 || parsed > 1000000) return false;
    out = static_cast<int>(parsed);
    return true;
}

/// Parses a port-only value, rejecting anything that smells like a host.
bool parse_port(const std::string& text, std::uint16_t& port) {
    if (text.find(':') != std::string::npos) return false;
    std::string host = "127.0.0.1";
    if (!parse_host_port(text, host, port)) return false;
    return true;
}

/// True when the bind address is reachable only from this machine.
bool loopback_only(const std::string& host) {
    return host == "127.0.0.1" || host == "localhost" || host == "::1" || host == "[::1]";
}

/// The default --fs-root: the home directory of whoever started this. $HOME
/// when it is set, because that is what a shell means by `~`, and the passwd
/// entry otherwise. Empty when neither can be had, in which case the file
/// tools stay off rather than run with no root to stay inside.
std::string default_fs_root() {
    if (const char* home = std::getenv("HOME"); home != nullptr && home[0] != '\0') return home;
    if (const struct passwd* entry = ::getpwuid(::getuid());
        entry != nullptr && entry->pw_dir != nullptr && entry->pw_dir[0] != '\0') {
        return entry->pw_dir;
    }
    return {};
}

Options parse_args(int argc, char** argv) {
    // The bare-invocation defaults, so that every flag below reads as taking
    // something away rather than granting it. The transport and tool layers
    // keep their own conservative defaults; it is this command line that says
    // what `ggmcp` with no arguments means.
    Options options;
    options.http.allow_any_origin = true;

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
            // Naming an origin is a choice, so it switches the any-Origin default
            // off: otherwise the list would be silently ignored.
            if (const char* v = next_value()) {
                options.http.allowed_origins.emplace_back(v);
                options.http.allow_any_origin = false;
            }
        } else if (arg.rfind("--allow-origin=", 0) == 0) {
            options.http.allowed_origins.push_back(arg.substr(15));
            options.http.allow_any_origin = false;
        } else if (arg == "--allow-any-origin") {
            options.http.allow_any_origin = true;
        } else if (arg == "--strict-origin") {
            options.http.allow_any_origin = false;
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
        } else if (arg == "--fs-root") {
            // --fs-root and --no-file-tools are the two halves of one decision,
            // so whichever comes last wins.
            if (const char* v = next_value()) {
                options.fs_root = v;
                options.fs_root_given = true;
                options.no_file_tools = false;
            }
        } else if (arg.rfind("--fs-root=", 0) == 0) {
            options.fs_root = arg.substr(10);
            options.fs_root_given = true;
            options.no_file_tools = false;
        } else if (arg == "--no-file-tools") {
            options.no_file_tools = true;
            options.fs_root.clear();
            options.fs_root_given = false;
        } else if (arg == "--allow-bash") {
            options.allow_bash = true;
        } else if (arg == "--no-bash") {
            options.allow_bash = false;
        } else if (arg == "--bash-timeout") {
            if (const char* v = next_value()) {
                if (!parse_int(v, options.bash_timeout)) {
                    options.error = "--bash-timeout expects a number of seconds, got '" + std::string(v) + "'";
                    return options;
                }
            }
        } else if (arg.rfind("--bash-timeout=", 0) == 0) {
            if (!parse_int(arg.substr(15), options.bash_timeout)) {
                options.error = "--bash-timeout expects a number of seconds, got '" + arg.substr(15) + "'";
                return options;
            }
        } else if (arg == "--require-token") {
            options.require_token = true;
        } else if (arg == "--token-stdin") {
            options.token_stdin = true;
        } else if (arg == "--token-env") {
            if (const char* v = next_value()) options.auth.token_env.emplace_back(v);
        } else if (arg.rfind("--token-env=", 0) == 0) {
            options.auth.token_env.push_back(arg.substr(12));
        } else if (arg == "--token-file") {
            if (const char* v = next_value()) options.auth.token_files.emplace_back(v);
        } else if (arg.rfind("--token-file=", 0) == 0) {
            options.auth.token_files.push_back(arg.substr(13));
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

    if (options.bash_timeout < 1 || options.bash_timeout > 3600) {
        options.error = "--bash-timeout must be between 1 and 3600 seconds";
        return options;
    }

    // Authorization is a property of the HTTP transport. Asking for it over stdio
    // means the flags were meant for a different invocation, so say so now rather
    // than silently ignoring the request.
    if (options.token_stdin && options.stdio) {
        options.error = "--token-stdin is meaningless with --stdio: stdio has no requests to authenticate";
        return options;
    }
    if (!options.require_token &&
        (!options.auth.token_env.empty() || !options.auth.token_files.empty() || options.token_stdin)) {
        options.error = "the token flags (--token-env, --token-file, --token-stdin) need --require-token";
        return options;
    }

    // The default file root is the home directory, so a bare invocation offers
    // read/write/edit over the files the person running it already owns. It is
    // resolved here rather than in the struct because --fs-root and
    // --no-file-tools both have to be settled first; an unresolvable home
    // leaves the tools off, which main() reports instead of failing over.
    if (!options.no_file_tools && !options.fs_root_given) options.fs_root = default_fs_root();

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

    ggmcp::FileSystemOptions filesystem;
    filesystem.enable_bash = options.allow_bash;
    filesystem.bash_timeout_seconds = options.bash_timeout;
    if (!options.fs_root.empty()) {
        std::error_code ec;
        const std::filesystem::path canonical = std::filesystem::canonical(options.fs_root, ec);
        if (ec) {
            if (options.fs_root_given) {
                std::fprintf(stderr, "%s: --fs-root \"%s\" cannot be resolved: %s\n",
                             ggmcp::kDefaultServerName, options.fs_root.c_str(), ec.message().c_str());
                return 2;
            }
            // The root is the default, not something the operator typed: say so
            // and serve the demo tools, rather than refusing to start over a
            // home directory that happens to be missing.
            ggmcp::log_line("warning: default file root \"" + options.fs_root + "\" cannot be resolved: " +
                            ec.message() + " - the file tools are off");
        } else {
            filesystem.root = canonical.string();
            filesystem.enable_file_tools = true;
        }
    }

    // Prepare credentials before anything listens. A server configured to demand
    // a token it was never handed must fail here, not on the first request.
    ggmcp::AuthOptions auth = options.auth;
    auth.required = options.require_token;
    if (options.token_stdin) {
        std::string line;
        if (!std::getline(std::cin, line)) {
            std::fprintf(stderr, "%s: --token-stdin but stdin is empty\n", ggmcp::kDefaultServerName);
            return 2;
        }
        while (!line.empty() && (line.back() == '\r' || line.back() == '\n')) line.pop_back();
        auth.token_literal = line;
    }

    // The handler is shared by every connection thread and TokenStore owns a
    // mutex, so it lives behind a shared_ptr built here and handed over once.
    std::shared_ptr<ggmcp::TokenStore> tokens;
    if (auth.required) {
        tokens = std::make_shared<ggmcp::TokenStore>();
        std::string auth_error;
        if (!ggmcp::TokenStore::prepare(auth, *tokens, auth_error)) {
            std::fprintf(stderr, "%s: %s\n", ggmcp::kDefaultServerName, auth_error.c_str());
            return 2;
        }
    }
    ggmcp::HttpOptions http = options.http;
    http.tokens = tokens;

    ggmcp::Dispatcher dispatcher(ggmcp::kDefaultServerName, ggmcp::kDefaultServerVersion);
    dispatcher.session_policy(options.session_policy);
    ggmcp::register_demo_tools(dispatcher);
    ggmcp::register_filesystem_tools(dispatcher, filesystem);

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
    server.set_handler(ggmcp::make_http_handler(dispatcher, http, server.port()));

    std::string tools;
    for (const auto& name : dispatcher.tool_names()) {
        if (!tools.empty()) tools += ", ";
        tools += name;
    }
    ggmcp::log_line(std::string(ggmcp::kDefaultServerName) + " " + ggmcp::kDefaultServerVersion +
                    " ready - tools: " + tools + " - session policy: " +
                    ggmcp::to_string(options.session_policy));
    if (tokens) {
        std::string sources;
        for (const std::string& label : tokens->labels()) {
            if (!sources.empty()) sources += ", ";
            sources += label;
        }
        ggmcp::log_line("authorization required - " + std::to_string(tokens->size()) + " token" +
                        (tokens->size() == 1 ? "" : "s") + " from " + sources +
                        "; unauthenticated requests get 401");
    } else if (!loopback_only(options.host)) {
        ggmcp::log_line("warning: bound to " + options.host + " with NO authorization - every tool here is "
                        "reachable from the network");
    } else if (filesystem.enable_file_tools || filesystem.enable_bash) {
        ggmcp::log_line("warning: no authorization - any process that can reach this port can call "
                        "these tools");
    }
    if (filesystem.enable_file_tools) ggmcp::log_line("file tools confined to " + filesystem.root);
    if (filesystem.enable_bash) ggmcp::log_line("bash is ENABLED - commands run with this process' privileges");
    if (http.enable_health_endpoint) ggmcp::log_line("GET /health reports status");
    ggmcp::log_line("press Ctrl-C to stop");

    const int status = server.serve();
    ggmcp::log_line("shutting down");
    return status;
}
