#include "gg/mcp/config.hpp"

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <sstream>
#include <string>

namespace gg::mcp {
namespace {

bool parseInt(const std::string& text, int& out) {
    if (text.empty()) {
        return false;
    }
    std::size_t consumed = 0;
    long long value = 0;
    try {
        value = std::stoll(text, &consumed);
    } catch (const std::exception&) {
        return false;
    }
    if (consumed != text.size() || value < -1 || value > 1000000000LL) {
        return false;
    }
    out = static_cast<int>(value);
    return true;
}

bool looksLikePath(const std::string& text) {
    return !text.empty() && text.front() == '/';
}

bool looksLikeOrigin(const std::string& text) {
    return text.find("://") != std::string::npos;
}

} // namespace

const char* toString(SessionPolicy policy) {
    switch (policy) {
    case SessionPolicy::Lenient: return "lenient";
    case SessionPolicy::Strict: return "strict";
    case SessionPolicy::Stateless: return "stateless";
    }
    return "lenient";
}

bool parseSessionPolicy(const std::string& text, SessionPolicy& policy) {
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

std::string sessionPolicyNames() {
    return "lenient, strict, stateless";
}

const char* toString(Transport transport) {
    switch (transport) {
    case Transport::Http: return "http";
    case Transport::Stdio: return "stdio";
    }
    return "http";
}

std::string Options::root() const {
    if (!fsRoot.empty()) {
        return fsRoot;
    }
    const char* home = std::getenv("HOME");
    if (home != nullptr && *home != '\0') {
        return home;
    }
    return "";
}

std::string usageText() {
    return "usage: ggmcp [--help] [--transport http|stdio] [--host ADDR] [--port N] "
           "[--endpoint PATH]\n"
           "              [--session-policy lenient|strict|stateless] [--fs-root PATH] "
           "[--allow-outside-root]\n"
           "              [--no-file-tools] [--no-bash] [--bash-timeout N] [--strict-origin] "
           "[--allow-origin ORIGIN]\n"
           "              [--require-token] [--token-env NAME | --token-file PATH | --token-stdin]\n"
           "              [--strict-origin] [--allow-origin ORIGIN] [--no-cors] "
           "[--bash-timeout N]\n"
           "              [--alias ALIAS] [--name NAME] [--title TITLE] [--instructions TEXT] "
           "[--json-backend NAME] [--http-backend NAME]\n"
           "              [--quiet] [--verbose]";
}

std::string helpText() {
    std::ostringstream out;
    out << "ggmcp - a Model Context Protocol server over streamable HTTP and stdio\n\n"
        << usageText() << "\n\n"
        << "Transports\n"
        << "  --transport http|stdio      streamable HTTP (default) or one message per line\n"
        << "  --host ADDR                 address to bind, default 0.0.0.0\n"
        << "  --port N                      port to bind, default 8081 (0 picks a free one)\n"
        << "  --endpoint PATH             JSON-RPC endpoint, default /mcp\n"
        << "  --session-policy POLICY     " << sessionPolicyNames() << ", default lenient\n"
        << "\nTools and confinement\n"
        << "  --fs-root PATH              root the file tools are confined to, default $HOME\n"
        << "  --allow-outside-root        drop file tool confinement\n"
        << "  --no-file-tools             do not offer read, write, edit\n"
        << "  --no-bash                   do not offer bash\n"
        << "  --bash-timeout N            bash timeout in seconds, default 30\n"
        << "\nOrigins\n"
        << "  --strict-origin             require the Origin header to match the Host or --allow-origin\n"
        << "  --allow-origin ORIGIN       allow an extra origin, repeatable, '*' means any\n"
        << "  --no-cors                   never send CORS headers, for non browser clients\n"
        << "\nAuthorization\n"
        << "  --require-token             require 'Authorization: Bearer <token>'\n"
        << "  --token-env NAME            read the token from NAME, default $GGMCP_TOKEN\n"
        << "  --token-file PATH           read the token from a file, re-read on change\n"
        << "  --token-stdin               read the token from the first line of stdin\n"
        << "\nServer info\n"
        << "  --alias ALIAS               publish tools as ALIAS-<tool>, default 'mcp', empty for none\n"
        << "  --name NAME                 serverInfo.name, default ggmcp\n"
        << "  --title TITLE               human readable title\n"
        << "  --instructions TEXT         instructions handed to clients\n"
        << "  --json-backend NAME         " << "nlohmann\n"
        << "  --http-backend NAME         httplib\n"
        << "  --quiet, --verbose          control stderr logging\n"
        << "\nJSON-RPC errors are answered at HTTP 200; non-2xx is reserved for transport faults.";
    return out.str();
}

bool parseOptions(int argc, char** argv, Options& options, std::string& error,
                  std::string& usage) {
    bool tokenEnvGiven = false;
    bool tokenFileGiven = false;
    bool tokenStdinGiven = false;

    const auto needValue = [&](const std::string& flag, int& index, int argcv, char** argvv,
                               std::string& out) {
        if (index + 1 >= argcv) {
            error = flag + " needs a value";
            return false;
        }
        out = argvv[++index];
        return true;
    };

    for (int index = 1; index < argc; ++index) {
        const std::string flag = argv[index];

        if (flag == "--help" || flag == "-h") {
            usage = helpText();
            return true;
        }
        if (flag == "--version") {
            usage = std::string("ggmcp ") + options.serverVersion;
            return true;
        } else if (flag == "--allow-outside-root") {
            options.allowOutsideRoot = true;
        } else if (flag == "--no-file-tools") {
            options.fileTools = false;
        } else if (flag == "--no-bash") {
            options.bashTool = false;
        } else if (flag == "--strict-origin") {
            options.strictOrigin = true;
        } else if (flag == "--no-cors") {
            options.cors = false;
        } else if (flag == "--alias") {
            std::string value;
            if (!needValue(flag, index, argc, argv, value)) {
                return false;
            }
            if (value.find_first_of(" \t/") != std::string::npos) {
                error = "--alias needs a single word without spaces or '/', got '" + value + "'";
                return false;
            }
            options.alias = value;
        } else if (flag == "--require-token") {
            options.requireToken = true;
        } else if (flag == "--token-stdin") {
            options.tokenStdin = true;
            tokenStdinGiven = true;
        } else if (flag == "--quiet") {
            options.quiet = true;
        } else if (flag == "--verbose") {
            options.verbose = true;
        } else if (flag == "--host") {
            if (!needValue(flag, index, argc, argv, options.host)) {
                return false;
            }
            if (options.host.empty()) {
                error = "--host needs an address";
                return false;
            }
        } else if (flag == "--port") {
            std::string value;
            if (!needValue(flag, index, argc, argv, value)) {
                return false;
            }
            int port = 0;
            if (!parseInt(value, port) || port < 0 || port > 65535) {
                error = "--port needs a number between 0 and 65535, got '" + value + "'";
                return false;
            }
            options.port = port;
        } else if (flag == "--endpoint") {
            if (!needValue(flag, index, argc, argv, options.endpoint)) {
                return false;
            }
            if (!looksLikePath(options.endpoint)) {
                error = "--endpoint needs an absolute path, got '" + options.endpoint + "'";
                return false;
            }
        } else if (flag == "--transport") {
            std::string value;
            if (!needValue(flag, index, argc, argv, value)) {
                return false;
            }
            if (value == "http" || value == "streamable-http" || value == "streamable http") {
                options.transport = Transport::Http;
            } else if (value == "stdio") {
                options.transport = Transport::Stdio;
            } else {
                error = "--transport needs 'http' or 'stdio', got '" + value + "'";
                return false;
            }
        } else if (flag == "--session-policy") {
            std::string value;
            if (!needValue(flag, index, argc, argv, value)) {
                return false;
            }
            if (!parseSessionPolicy(value, options.sessionPolicy)) {
                error = "--session-policy needs " + sessionPolicyNames() + ", got '" + value + "'";
                return false;
            }
        } else if (flag == "--fs-root") {
            std::string value;
            if (!needValue(flag, index, argc, argv, value)) {
                return false;
            }
            std::error_code code;
            if (!looksLikePath(value)) {
                error = "--fs-root needs an absolute path, got '" + value + "'";
                return false;
            }
            if (!std::filesystem::is_directory(value, code)) {
                error = "--fs-root '" + value + "' is not a directory";
                return false;
            }
            options.fsRoot = std::filesystem::weakly_canonical(value, code).string();
            if (options.fsRoot.empty()) {
                options.fsRoot = value;
            }
        } else if (flag == "--bash-timeout") {
            std::string value;
            if (!needValue(flag, index, argc, argv, value)) {
                return false;
            }
            int seconds = 0;
            if (!parseInt(value, seconds) || seconds < 1 || seconds > 3600) {
                error = "--bash-timeout needs a number between 1 and 3600, got '" + value + "'";
                return false;
            }
            options.bashTimeoutSeconds = seconds;
        } else if (flag == "--allow-origin") {
            std::string value;
            if (!needValue(flag, index, argc, argv, value)) {
                return false;
            }
            if (value != "*" && !looksLikeOrigin(value)) {
                error = "--allow-origin needs a scheme or '*', got '" + value + "'";
                return false;
            }
            options.allowedOrigins.push_back(value);
        } else if (flag == "--token-env") {
            if (!needValue(flag, index, argc, argv, options.tokenEnv)) {
                return false;
            }
            tokenEnvGiven = true;
            if (options.tokenEnv.empty()) {
                error = "--token-env needs a variable name";
                return false;
            }
        } else if (flag == "--token-file") {
            if (!needValue(flag, index, argc, argv, options.tokenFile)) {
                return false;
            }
            tokenFileGiven = true;
            if (options.tokenFile.empty()) {
                error = "--token-file needs a path";
                return false;
            }
        } else if (flag == "--name") {
            if (!needValue(flag, index, argc, argv, options.serverName)) {
                return false;
            }
            if (options.serverName.empty()) {
                error = "--name needs a value";
                return false;
            }
        } else if (flag == "--title") {
            if (!needValue(flag, index, argc, argv, options.serverTitle)) {
                return false;
            }
        } else if (flag == "--instructions") {
            if (!needValue(flag, index, argc, argv, options.instructions)) {
                return false;
            }
        } else if (flag == "--json-backend") {
            if (!needValue(flag, index, argc, argv, options.jsonBackend)) {
                return false;
            }
        } else if (flag == "--http-backend") {
            if (!needValue(flag, index, argc, argv, options.httpBackend)) {
                return false;
            }
        } else if (!flag.empty() && flag.front() == '-') {
            error = "unknown option '" + flag + "'";
            usage = usageText();
            return false;
        } else {
            error = "unexpected argument '" + flag + "'";
            usage = usageText();
            return false;
        }
    }

    if (tokenEnvGiven && tokenFileGiven) {
        error = "--token-env and --token-file cannot both be used";
        return false;
    }
    if (tokenStdinGiven && (tokenEnvGiven || tokenFileGiven)) {
        error = "--token-stdin cannot be combined with --token-env or --token-file";
        return false;
    }
    if (!options.requireToken && (tokenEnvGiven || tokenFileGiven || tokenStdinGiven)) {
        error = "a token source needs --require-token";
        return false;
    }
    if (options.requireToken && options.transport == Transport::Stdio) {
        error = "--require-token only applies to the http transport";
        return false;
    }
    if (options.requireToken && !tokenEnvGiven && !tokenFileGiven && !tokenStdinGiven) {
        options.tokenEnv = "GGMCP_TOKEN";
    }
    if (options.requireToken && options.tokenFile == "-") {
        error = "--token-file - is not supported, use --token-stdin";
        return false;
    }
    return true;
}

} // namespace gg::mcp
