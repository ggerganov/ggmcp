// Command line options and their validation.

#pragma once

#include <string>
#include <vector>

namespace gg::mcp {

enum class SessionPolicy {
    Lenient,   // requests without a session id run on an ephemeral session
    Strict,    // an initialized session id is required after the handshake
    Stateless, // no session is ever issued or tracked
};

const char* toString(SessionPolicy policy);
bool parseSessionPolicy(const std::string& text, SessionPolicy& policy);
std::string sessionPolicyNames();

enum class Transport {
    Http,
    Stdio,
};

const char* toString(Transport transport);

struct Options {
    std::string host = "0.0.0.0";
    int port = 8081;
    std::string endpoint = "/mcp";
    std::string healthPath = "/health";
    std::string indexPath = "/";
    std::string metadataPath = "/.well-known/oauth-protected-resource";
    std::string serverName = "ggmcp";
    std::string serverTitle = "ggmcp";
    std::string serverVersion = GGMCP_VERSION;
    std::string alias = "mcp";               // published tool names are "<alias>-<tool>"
    std::string instructions = "Use the tools to read, write and edit files under the root.";

    Transport transport = Transport::Http;
    SessionPolicy sessionPolicy = SessionPolicy::Lenient;

    std::string fsRoot;            // empty until resolved, defaults to $HOME
    bool allowOutsideRoot = false;

    bool fileTools = true;
    bool bashTool = true;
    int bashTimeoutSeconds = 30;

    bool strictOrigin = false;
    bool cors = true;
    std::vector<std::string> allowedOrigins;

    bool requireToken = false;
    std::string tokenEnv = "GGMCP_TOKEN";
    std::string tokenFile;
    bool tokenStdin = false;

    std::string jsonBackend = "nlohmann";
    std::string httpBackend = "httplib";

    bool quiet = false;
    bool verbose = false;

    std::string root() const; // $HOME when --fs-root was not given
};

// Returns false with a message for the user when the command line is not usable.
// `usage` is set when --help or --version was requested.
bool parseOptions(int argc, char** argv, Options& options, std::string& error,
                  std::string& usage);

std::string usageText();
std::string helpText();

} // namespace gg::mcp
