// ggmcp - a tiny Model Context Protocol server in dependency-free C++17.
//
// SPDX-License-Identifier: MIT
//
// protocol.hpp - MCP 2025-06-18 request handling, independent of transport.
//
// The dispatcher knows nothing about sockets or pipes: it takes a JSON-RPC
// message body and returns a Reply. Both the HTTP and stdio transports feed
// the same object, which is what keeps the two behaviours from drifting.

#ifndef GGGMCP_PROTOCOL_HPP
#define GGGMCP_PROTOCOL_HPP

#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

#include "ggmcp/json.hpp"

namespace ggmcp {

/// JSON-RPC 2.0 reserved codes plus the two MCP adds.
namespace error_code {
constexpr int kParseError      = -32700;
constexpr int kInvalidRequest  = -32600;
constexpr int kMethodNotFound  = -32601;
constexpr int kInvalidParams   = -32602;
constexpr int kInternalError   = -32603;
constexpr int kNotInitialized  = -32002;
}  // namespace error_code

/// Protocol versions this server will agree to speak. Newest last.
inline const std::vector<std::string>& supported_protocol_versions() {
    static const std::vector<std::string> kVersions = {"2024-11-05", "2025-03-26", "2025-06-18"};
    return kVersions;
}
inline const std::string& latest_protocol_version() { return supported_protocol_versions().back(); }

/// How hard the server insists on the `Mcp-Session-Id` header.
///
/// The spec makes session ids optional for the *server* ("the server MAY assign
/// an MCP session ID"), so a client that never got one - or that gets one and
/// cannot retain it - is still a legal caller. `Lenient` is the default for that
/// reason: it keeps full session behaviour for well-behaved clients while never
/// locking out a client that drops the header.
enum class SessionPolicy {
    Lenient,     ///< unknown/missing ids fall back to an implicit session (default)
    Strict,      ///< every request after initialize must carry a known id
    Stateless,   ///< never issue or honour session ids; each request stands alone
};

inline const char* to_string(SessionPolicy policy) {
    switch (policy) {
        case SessionPolicy::Lenient:   return "lenient";
        case SessionPolicy::Strict:    return "strict";
        case SessionPolicy::Stateless: return "stateless";
    }
    return "lenient";
}

/// What a tool hands back to the client.
struct ToolResult {
    Array content;              ///< array of content blocks, e.g. {"type":"text","text":...}
    bool is_error = false;      ///< true => structured tool failure, still HTTP 200

    static ToolResult text(std::string body) {
        ToolResult r;
        Object block;
        block["type"] = Value::make_string("text");
        block["text"] = Value::make_string(std::move(body));
        r.content.push_back(Value::make_object(std::move(block)));
        return r;
    }

    static ToolResult json(std::string serialized) {
        ToolResult r;
        Object block;
        block["type"] = Value::make_string("text");
        block["text"] = Value::make_string(std::move(serialized));
        block["mimeType"] = Value::make_string("application/json");
        r.content.push_back(Value::make_object(std::move(block)));
        return r;
    }

    static ToolResult error(std::string message) {
        ToolResult r = ToolResult::text(std::move(message));
        r.is_error = true;
        return r;
    }
};

/// Per-call metadata. Handy for tools that want to report how they were reached.
struct Context {
    std::string transport;       ///< "http" or "stdio"
    std::string session_id;      ///< HTTP session id; empty over stdio
    std::string peer;            ///< "127.0.0.1:54321" for HTTP, "-" for stdio
    std::string client_name;     ///< from clientInfo
    std::string client_version;
};

struct Tool {
    std::string name;
    std::string description;
    Value input_schema = Value::make_object();   ///< JSON Schema for `arguments`
    Value annotations = Value::make_null();      ///< MCP tool annotations, optional
    std::function<ToolResult(const Value& arguments, const Context& ctx)> handler;
};

/// Builds JSON Schema fragments for tool input schemas.
namespace schema {
Value string(const std::string& description, const std::vector<std::string>& allowed = {});
Value integer(const std::string& description);
Value boolean(const std::string& description);
Value object(std::map<std::string, Value> properties, std::vector<std::string> required = {});
Value array(const std::string& description, Value items, std::int64_t min_items = 1);
Value string_array(const std::string& description, std::int64_t min_items = 0);
}  // namespace schema

/// One client handshake. HTTP transports look these up by Mcp-Session-Id.
struct Session {
    std::string id;
    std::string protocol_version;
    std::string client_name;
    std::string client_version;
    bool initialized = false;    ///< saw notifications/initialized
};

/// A transport-facing answer. `status` is meaningful for HTTP and ignored by
/// stdio; `has_body == false` means "send the status, then nothing" (202).
struct Reply {
    int status = 200;
    bool has_body = true;
    std::string body;            ///< serialized JSON-RPC message
    std::string session_id;      ///< set when the caller should echo Mcp-Session-Id
    std::string content_type = "application/json";

    static Reply ok(std::string json_body)        { Reply r; r.body = std::move(json_body); return r; }
    static Reply accepted()                       { Reply r; r.status = 202; r.has_body = false; r.content_type.clear(); return r; }
};

/// Implements initialize / ping / tools.list / tools.call and owns session state.
class Dispatcher {
public:
    Dispatcher(std::string server_name, std::string server_version);
    ~Dispatcher();

    Dispatcher(const Dispatcher&) = delete;
    Dispatcher& operator=(const Dispatcher&) = delete;

    void add_tool(Tool tool);
    std::size_t tool_count() const;
    bool has_tool(const std::string& name) const;
    std::vector<std::string> tool_names() const;

    /// Session strictness. Set before serving; read-only afterwards.
    void session_policy(SessionPolicy policy) { session_policy_ = policy; }
    SessionPolicy session_policy() const { return session_policy_; }

    /// Handles one complete JSON-RPC message. Never throws.
    /// `session_id` names an existing session when the transport has one.
    Reply handle(std::string_view message, Context ctx);

    /// Session store, exposed for transport teardown and status output.
    bool session_exists(const std::string& id) const;
    std::size_t session_count() const;
    void destroy_session(const std::string& id);

    const std::string& server_name() const { return name_; }
    const std::string& server_version() const { return version_; }

private:
    Value error_object(int code, const std::string& message, const Value& id) const;
    Reply make_result(Value result, const Value& id) const;

    /// A well-formed JSON-RPC message that carries an error member. Goes out at
    /// HTTP 200 so clients surface `error.message` instead of a bare status line.
    Reply rpc_error(int code, const std::string& message, const Value& id) const;

    /// A transport-level rejection (unparseable, wrong framing, unknown
    /// session): the HTTP status itself carries the meaning.
    Reply http_error(int status, int code, const std::string& message, const Value& id) const;
    Reply dispatch(const Value& request, const Value& id, bool has_id, Context& ctx);
    Reply on_initialize(const Value& params, const Value& id, Context& ctx);
    Reply on_tools_list(const Value& params, const Value& id) const;
    Reply on_tools_call(const Value& params, const Value& id, Context& ctx) const;
    std::string new_session_id() const;

    std::string name_;
    std::string version_;
    std::vector<Tool> tools_;

    mutable std::mutex mutex_;
    std::map<std::string, Session> sessions_;
    SessionPolicy session_policy_ = SessionPolicy::Lenient;
};

/// Accepts "lenient", "strict" or "stateless", case-insensitively.
bool parse_session_policy(const std::string& text, SessionPolicy& out);

/// Test/demo helper: a stdio dispatcher loop. Returns on EOF.
int serve_stdio(Dispatcher& dispatcher);

}  // namespace ggmcp

#endif  // GGGMCP_PROTOCOL_HPP
