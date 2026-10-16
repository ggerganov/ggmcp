// MCP protocol handling. Transport agnostic: the HTTP and stdio transports
// collect an Inbound, hand it over and get an Outbound back.

#pragma once

#include <string>
#include <vector>

#include "gg/json.hpp"
#include "gg/mcp/config.hpp"
#include "gg/mcp/session.hpp"
#include "gg/mcp/tools.hpp"

namespace gg::mcp {

namespace protocol {

inline constexpr const char* kVersion20241105 = "2024-11-05";
inline constexpr const char* kVersion20250326 = "2025-03-26";
inline constexpr const char* kVersion20250618 = "2025-06-18";

inline constexpr const char* kSessionIdHeader = "Mcp-Session-Id";
inline constexpr const char* kProtocolVersionHeader = "MCP-Protocol-Version";

std::vector<std::string> supportedVersions();
std::string newestVersion();
bool isSupported(const std::string& version);
std::string negotiate(const std::string& requested);

Json error(const Json& id, int code, const std::string& message, const Json& data = Json());
Json result(const Json& id, const Json& payload);

// Transport faults are plain payloads, not JSON-RPC errors: JSON-RPC errors
// always travel at HTTP 200.
Json transportFault(const std::string& code, const std::string& message);

namespace error_code {
inline constexpr int kParseError = -32700;
inline constexpr int kInvalidRequest = -32600;
inline constexpr int kMethodNotFound = -32601;
inline constexpr int kInvalidParams = -32602;
inline constexpr int kInternalError = -32603;
} // namespace error_code

} // namespace protocol

// Everything the protocol layer needs about one incoming message.
struct Inbound {
    std::string transport = "http";
    std::string body;
    std::string sessionId;
    std::string contentType;
    std::string accept;
    std::string origin;
    std::string userAgent;
    std::string clientAddress;
    std::string protocolVersionHeader;
};

struct Outbound {
    int status = 200;
    bool hasBody = true;
    Json payload; // JSON-RPC response, or a transport fault payload
    std::string contentType = "application/json";
    std::string sessionId; // session id to emit, empty when there is none
    std::string method;    // the JSON-RPC method that was handled, for logging

    static Outbound jsonRpc(int status, Json payload);
    static Outbound fault(int status, const std::string& code, const std::string& message);
    static Outbound accepted(); // notification handled, nothing to send back
};

class Protocol {
public:
    Protocol(Options options, ToolRegistry& tools, SessionStore& sessions);

    // One message. `inbound.body` holds the JSON-RPC text.
    Outbound handle(const Inbound& inbound);

    // stdio convenience: returns the reply to print, empty when there is none.
    std::string handleLine(const std::string& line);

    // Tear a session down (HTTP DELETE). Returns the status to report.
    int destroy(const std::string& sessionId);

    SessionStore& sessions() { return sessions_; }
    const Options& options() const { return options_; }

private:
    Outbound dispatch(const Json& message, const Json& id, const std::string& method,
                      const Inbound& inbound, const Session* session);

    Outbound handleInitialize(const Json& id, const Json& params, const Inbound& inbound);
    Outbound handleToolsCall(const Json& id, const Json& params, const Inbound& inbound,
                             const Session* session);

    ToolContext toolContext(const Inbound& inbound, const Session* session) const;

    Options options_;
    ToolRegistry& tools_;
    SessionStore& sessions_;
};

} // namespace gg::mcp
