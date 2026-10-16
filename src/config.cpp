#include "ggmcp/config.hpp"

#include <cstdlib>
#include <filesystem>
#include <system_error>

#include "ggmcp/paths.hpp"
#include "ggmcp/version.hpp"

namespace ggmcp {
namespace {

bool numeric(const std::string& text, unsigned long long& out) {
    if (text.empty()) return false;
    for (char c : text) {
        if (c < '0' || c > '9') return false;
    }
    errno = 0;
    char* end = nullptr;
    const char* begin = text.c_str();
    out = std::strtoull(begin, &end, 10);
    return end == begin + text.size() && errno != ERANGE;
}

bool alias_is_sane(const std::string& alias) {
    for (char c : alias) {
        const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                        (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-';
        if (!ok) return false;
    }
    return true;
}

bool endpoint_is_sane(const std::string& endpoint) {
    if (endpoint.size() < 2 || endpoint.front() != '/' || endpoint.back() == '/') {
        return false;
    }
    for (char c : endpoint) {
        if (c <= ' ' || c == '?' || c == '#') return false;
    }
    return true;
}

bool has_no_newline(const std::string& text) {
    return text.find('\n') == std::string::npos && text.find('\r') == std::string::npos;
}

}  // namespace

const char* to_string(Transport transport) {
    return transport == Transport::Stdio ? "stdio" : "http";
}

const char* to_string(SessionPolicy policy) {
    switch (policy) {
        case SessionPolicy::Lenient: return "lenient";
        case SessionPolicy::Strict: return "strict";
        case SessionPolicy::Stateless: return "stateless";
    }
    return "lenient";
}

bool parse_session_policy(const std::string& text, SessionPolicy& policy) {
    if (text == "lenient") {
        policy = SessionPolicy::Lenient;
        return true;
    }
    if (text == "strict") {
        policy = SessionPolicy::Strict;
        return true;
    }
    if (text == "stateless") {
        policy = SessionPolicy::Stateless;
        return true;
    }
    return false;
}

bool parse_transport(const std::string& text, Transport& transport) {
    if (text == "http" || text == "streamable-http") {
        transport = Transport::Http;
        return true;
    }
    if (text == "stdio") {
        transport = Transport::Stdio;
        return true;
    }
    return false;
}

std::string Config::url() const {
    std::string address = host;
    if (address == "0.0.0.0") address = "127.0.0.1";
    if (!address.empty() && address.front() != '[' && address.find(':') != std::string::npos) {
        address = "[" + address + "]";
    }
    return "http://" + address + ":" + std::to_string(port) + endpoint;
}

std::string Config::root_description() const {
    if (allow_outside_root) return fs_root + " (not confined)";
    return fs_root + " (confined)";
}

std::string help_text() {
    return std::string() +
           "ggmcp " + kVersion + " - a Model Context Protocol server\n"
           "\n"
           "Usage: ggmcp [flag ...]\n"
           "\n"
           "Process\n"
           "  -h, --help              print this and exit 0\n"
           "      --version           print the version and exit 0\n"
           "  -q, --quiet             say nothing at start up\n"
           "\n"
           "Transport\n"
           "      --transport T       http | stdio (default: http)\n"
           "      --host HOST         address to bind (default: 0.0.0.0)\n"
           "      --port N            port to bind (default: 8081)\n"
           "      --endpoint PATH     JSON-RPC path (default: /mcp)\n"
           "      --max-body N        request body limit, bytes (default: 1048576)\n"
           "      --session-policy P  lenient | strict | stateless (default: lenient)\n"
           "\n"
           "Server info\n"
           "      --name NAME         serverInfo.name (default: ggmcp)\n"
           "      --title TITLE       serverInfo.title (default: the value of --name)\n"
           "      --instructions TEXT initialize instructions (default: none)\n"
           "      --alias ALIAS       tool name prefix (default: mcp; empty for none)\n"
           "\n"
           "Tools\n"
           "      --no-file-tools     drop read, write and edit\n"
           "      --no-bash           drop bash\n"
           "\n"
           "File tools\n"
           "      --fs-root PATH      confine the file tools (default: $HOME)\n"
           "      --allow-outside-root  take every path as given\n"
           "\n"
           "Authorization\n"
           "      --require-token     serve as a bearer-token resource server\n"
           "      --token-env NAME    read the token from $NAME (default: $GGMCP_TOKEN)\n"
           "      --token-file PATH   read the token from a file, hot reloaded\n"
           "      --token-stdin       read the token from the first line of stdin\n"
           "\n"
           "Browser clients\n"
           "      --allow-origin ORIGIN  allow one origin, repeatable (* for any)\n"
           "      --strict-origin     refuse origins off the Host and --allow-origin list\n"
           "      --no-cors           send no CORS headers\n"
           "\n"
           "Endpoints\n"
           "      GET  <endpoint>     405 - this server does not stream\n"
           "      POST <endpoint>     JSON-RPC\n"
           "      DELETE <endpoint>   close a session\n"
           "      GET  /health        what this instance is\n"
           "      GET  /              index\n"
           "      GET  /.well-known/oauth-protected-resource   RFC 9728 metadata\n";
}

ParseResult parse_args(int argc, char** argv, Config& cfg, std::string& error) {
    auto need_value = [&](int& i, const std::string& flag, std::string& out) -> bool {
        if (i + 1 >= argc) {
            error = flag + " needs a value";
            return false;
        }
        out = argv[++i];
        return true;
    };

    bool token_source_set = false;

    for (int i = 1; i < argc; ++i) {
        const std::string flag = argv[i];
        const std::string value = i + 1 < argc ? argv[i + 1] : std::string();
        (void)value;

        if (flag == "--help" || flag == "-h") return ParseResult::Help;
        if (flag == "--version") return ParseResult::Version;

        if (flag == "--quiet" || flag == "-q") {
            cfg.quiet = true;
            continue;
        }
        if (flag == "--allow-outside-root") {
            cfg.allow_outside_root = true;
            continue;
        }
        if (flag == "--no-file-tools") {
            cfg.file_tools = false;
            continue;
        }
        if (flag == "--no-bash") {
            cfg.bash_tool = false;
            continue;
        }
        if (flag == "--require-token") {
            cfg.require_token = true;
            continue;
        }
        if (flag == "--token-stdin") {
            if (token_source_set) {
                error = "only one token source at a time";
                return ParseResult::Error;
            }
            cfg.token_stdin = true;
            token_source_set = true;
            continue;
        }
        if (flag == "--strict-origin") {
            cfg.strict_origin = true;
            continue;
        }
        if (flag == "--no-cors") {
            cfg.cors = false;
            continue;
        }

        std::string out;
        if (flag == "--host") {
            if (!need_value(i, flag, out)) return ParseResult::Error;
            if (out.empty() || !has_no_newline(out)) {
                error = "--host wants an address";
                return ParseResult::Error;
            }
            cfg.host = out;
            continue;
        }
        if (flag == "--port") {
            if (!need_value(i, flag, out)) return ParseResult::Error;
            unsigned long long port = 0;
            if (!numeric(out, port) || port < 1 || port > 65535) {
                error = "--port wants 1..65535, got '" + out + "'";
                return ParseResult::Error;
            }
            cfg.port = static_cast<int>(port);
            continue;
        }
        if (flag == "--max-body") {
            if (!need_value(i, flag, out)) return ParseResult::Error;
            unsigned long long size = 0;
            if (!numeric(out, size) || size < 16) {
                error = "--max-body wants at least 16 bytes, got '" + out + "'";
                return ParseResult::Error;
            }
            cfg.max_body = static_cast<std::size_t>(size);
            continue;
        }
        if (flag == "--transport") {
            if (!need_value(i, flag, out)) return ParseResult::Error;
            Transport transport = cfg.transport;
            if (!parse_transport(out, transport)) {
                error = "--transport wants http or stdio, got '" + out + "'";
                return ParseResult::Error;
            }
            cfg.transport = transport;
            continue;
        }
        if (flag == "--session-policy") {
            if (!need_value(i, flag, out)) return ParseResult::Error;
            SessionPolicy policy = cfg.session_policy;
            if (!parse_session_policy(out, policy)) {
                error = "--session-policy wants lenient, strict or stateless, got '" + out +
                        "'";
                return ParseResult::Error;
            }
            cfg.session_policy = policy;
            continue;
        }
        if (flag == "--endpoint") {
            if (!need_value(i, flag, out)) return ParseResult::Error;
            if (!endpoint_is_sane(out)) {
                error = "--endpoint wants an absolute path without a trailing slash, got '" +
                        out + "'";
                return ParseResult::Error;
            }
            cfg.endpoint = out;
            continue;
        }
        if (flag == "--name") {
            if (!need_value(i, flag, out)) return ParseResult::Error;
            if (out.empty() || !has_no_newline(out)) {
                error = "--name wants a name";
                return ParseResult::Error;
            }
            cfg.name = out;
            continue;
        }
        if (flag == "--title") {
            if (!need_value(i, flag, out)) return ParseResult::Error;
            if (!has_no_newline(out)) {
                error = "--title wants a single line";
                return ParseResult::Error;
            }
            cfg.title = out;
            continue;
        }
        if (flag == "--instructions") {
            if (!need_value(i, flag, out)) return ParseResult::Error;
            cfg.instructions = out;
            continue;
        }
        if (flag == "--alias") {
            if (!need_value(i, flag, out)) return ParseResult::Error;
            if (!alias_is_sane(out)) {
                error = "--alias wants letters, digits, dot, underscore or dash, got '" + out +
                        "'";
                return ParseResult::Error;
            }
            cfg.alias = out;
            continue;
        }
        if (flag == "--fs-root") {
            if (!need_value(i, flag, out)) return ParseResult::Error;
            std::error_code ec;
            std::filesystem::path root = std::filesystem::absolute(out, ec);
            if (ec || !std::filesystem::is_directory(root)) {
                error = "--fs-root wants a readable directory, got '" + out + "'";
                return ParseResult::Error;
            }
            root = std::filesystem::weakly_canonical(root, ec);
            if (ec) {
                error = "--fs-root wants a readable directory, got '" + out + "'";
                return ParseResult::Error;
            }
            cfg.fs_root = root.string();
            continue;
        }
        if (flag == "--token-env") {
            if (token_source_set) {
                error = "only one token source at a time";
                return ParseResult::Error;
            }
            if (!need_value(i, flag, out)) return ParseResult::Error;
            if (out.empty() || out.front() == '-' || !has_no_newline(out)) {
                error = "--token-env wants an environment variable name";
                return ParseResult::Error;
            }
            cfg.token_env = out;
            token_source_set = true;
            continue;
        }
        if (flag == "--token-file") {
            if (token_source_set) {
                error = "only one token source at a time";
                return ParseResult::Error;
            }
            if (!need_value(i, flag, out)) return ParseResult::Error;
            if (out.empty()) {
                error = "--token-file wants a path";
                return ParseResult::Error;
            }
            cfg.token_file = out;
            token_source_set = true;
            continue;
        }
        if (flag == "--allow-origin") {
            if (!need_value(i, flag, out)) return ParseResult::Error;
            if (out.empty() || !has_no_newline(out)) {
                error = "--allow-origin wants an origin, or * for any";
                return ParseResult::Error;
            }
            cfg.allow_origin.push_back(out);
            continue;
        }

        if (!flag.empty() && flag.front() == '-') {
            error = "unknown flag: " + flag;
            return ParseResult::Error;
        }
        error = "unexpected argument: " + flag;
        return ParseResult::Error;
    }

    if (cfg.fs_root.empty()) {
        cfg.fs_root = home_dir();
    }
    if (cfg.token_env.empty() && cfg.token_file.empty() && !cfg.token_stdin) {
        cfg.token_env = "GGMCP_TOKEN";
    }
    return ParseResult::Ok;
}

}  // namespace ggmcp
