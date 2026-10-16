#pragma once

#include <atomic>
#include <string>

#include "ggmcp/json.hpp"

namespace ggmcp {

struct Config;
class SessionStore;
class ToolRegistry;

// Oldest first. `initialize` offers the newest.
extern const char* const kProtocolVersions[3];
extern const char* const kLatestProtocolVersion;

bool knows_protocol_version(const std::string& version);

// Where a request came from. Both transports fill this in; the file tools and
// `whoami` read it back out.
struct RequestEnvelope {
    std::string transport = "http";
    std::string address;
    std::string session_id;              // Mcp-Session-Id as sent, empty when absent
    std::string protocol_version;        // MCP-Protocol-Version as sent, may be empty
};

// What a transport should put on the wire.
struct Outcome {
    int status = 200;
    bool has_body = true;
    Json body;                 // a JSON-RPC envelope, or a protocol fault object
    std::string session_id;    // worth echoing back in Mcp-Session-Id
};

// A JSON-RPC envelope always goes out at 200. A protocol fault goes out at a
// non-2xx with `error` as a string. A routing fault goes out at a non-2xx with
// `error` as the status number; only the transports raise those.
Json rpc_result(const Json& id, const Json& result);
Json rpc_error(const Json& id, int code, const std::string& message, const Json& data = Json());
Json protocol_fault(const std::string& code, const std::string& message);
Json routing_fault(int status, const std::string& message);

constexpr int kRpcParseError = -32700;
constexpr int kRpcInvalidRequest = -32600;
constexpr int kRpcMethodNotFound = -32601;
constexpr int kRpcInvalidParams = -32602;
constexpr int kRpcInternalError = -32603;

// Everything MCP, minus the transport: the handshake, the session policy, the
// method table and the tool call.
class Core {
public:
    Core(const Config& config, SessionStore& sessions, const ToolRegistry& tools);
    ~Core();

    // Stop a serving loop. Safe to call from a signal handler: it sets a flag and
    // pokes a self-pipe, so a loop blocked on a read wakes up.
    void stop();
    bool stopping() const { return stopping_.load(); }
    // A descriptor that turns readable when stop() was called, or -1.
    int wakeup() const { return wakeup_[0]; }

    Outcome handle_body(const std::string& body, const RequestEnvelope& env) const;

    // `DELETE`: close a session. Never dispatches.
    Outcome close_session(const std::string& session_id) const;

    Json server_info() const;
    Json health() const;

private:
    Outcome gate(const std::string& method, const RequestEnvelope& env) const;
    Outcome dispatch(const std::string& method, const Json& id, const Json& params,
                     const RequestEnvelope& env, std::string* issued) const;

    const Config* config_;
    SessionStore* sessions_;
    const ToolRegistry* tools_;
    int wakeup_[2] = {-1, -1};
    std::atomic<bool> stopping_{false};
};

}  // namespace ggmcp
