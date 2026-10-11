// ggmcp - a tiny Model Context Protocol server in dependency-free C++17.
//
// SPDX-License-Identifier: MIT

#include "ggmcp/protocol.hpp"

#include "ggmcp/log.hpp"

#include <algorithm>
#include <cstdio>
#include <iostream>
#include <random>
#include <sstream>

namespace ggmcp {
namespace {

void hex_rand(std::string& out, std::size_t bytes) {
    static thread_local std::mt19937_64 rng{std::random_device{}()};
    static constexpr char kHex[] = "0123456789abcdef";
    out.clear();
    out.reserve(bytes * 2);
    for (std::size_t i = 0; i < bytes; ++i) {
        const auto b = static_cast<unsigned>(rng() & 0xFF);
        out.push_back(kHex[b >> 4]);
        out.push_back(kHex[b & 0x0F]);
    }
}

}  // namespace

// ---------------------------------------------------------------------------
// schema builders
// ---------------------------------------------------------------------------

namespace schema {

Value string(const std::string& description, const std::vector<std::string>& allowed) {
    Object v;
    v["type"] = Value::make_string("string");
    if (!description.empty()) v["description"] = Value::make_string(description);
    if (!allowed.empty()) {
        Array values;
        for (const auto& a : allowed) values.push_back(Value::make_string(a));
        v["enum"] = Value::make_array(std::move(values));
    }
    return Value::make_object(std::move(v));
}

Value integer(const std::string& description) {
    Object v;
    v["type"] = Value::make_string("integer");
    if (!description.empty()) v["description"] = Value::make_string(description);
    return Value::make_object(std::move(v));
}

Value boolean(const std::string& description) {
    Object v;
    v["type"] = Value::make_string("boolean");
    if (!description.empty()) v["description"] = Value::make_string(description);
    return Value::make_object(std::move(v));
}

Value object(std::map<std::string, Value> properties, std::vector<std::string> required) {
    Object v;
    v["type"] = Value::make_string("object");
    Object props;
    for (auto&& kv : properties) props[kv.first] = kv.second;
    v["properties"] = Value::make_object(std::move(props));
    Array req;
    for (auto&& r : required) req.push_back(Value::make_string(std::move(r)));
    v["required"] = Value::make_array(std::move(req));
    // Most MCP clients validate arguments against this schema; being explicit
    // about extra keys avoids "additional properties" surprises.
    v["additionalProperties"] = Value::make_bool(false);
    return Value::make_object(std::move(v));
}

}  // namespace schema

// ---------------------------------------------------------------------------
// Dispatcher
// ---------------------------------------------------------------------------

Dispatcher::Dispatcher(std::string server_name, std::string server_version)
    : name_(std::move(server_name)), version_(std::move(server_version)) {}

Dispatcher::~Dispatcher() = default;

void Dispatcher::add_tool(Tool tool) {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto existing = std::find_if(tools_.begin(), tools_.end(),
                                       [&](const Tool& t) { return t.name == tool.name; });
    if (existing != tools_.end()) {
        log_line("warning: replacing duplicate tool '" + tool.name + "'");
        *existing = std::move(tool);
        return;
    }
    tools_.push_back(std::move(tool));
}

std::size_t Dispatcher::tool_count() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return tools_.size();
}

std::vector<std::string> Dispatcher::tool_names() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<std::string> names;
    names.reserve(tools_.size());
    for (const auto& t : tools_) names.push_back(t.name);
    return names;
}

bool Dispatcher::session_exists(const std::string& id) const {
    std::lock_guard<std::mutex> lock(mutex_);
    return sessions_.count(id) != 0;
}

std::size_t Dispatcher::session_count() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return sessions_.size();
}

void Dispatcher::destroy_session(const std::string& id) {
    std::lock_guard<std::mutex> lock(mutex_);
    sessions_.erase(id);
}

std::string Dispatcher::new_session_id() const {
    std::string id;
    for (int attempt = 0; attempt < 8; ++attempt) {
        hex_rand(id, 16);
        std::lock_guard<std::mutex> lock(mutex_);
        if (!sessions_.count(id)) return id;
    }
    return id;
}

Value Dispatcher::error_object(int code, const std::string& message, const Value& id) const {
    Object error;
    error["code"] = Value::make_int(code);
    error["message"] = Value::make_string(message);
    Object response;
    response["jsonrpc"] = Value::make_string("2.0");
    response["id"] = id;
    response["error"] = Value::make_object(std::move(error));
    return Value::make_object(std::move(response));
}

Reply Dispatcher::make_result(Value result, const Value& id) const {
    Object response;
    response["jsonrpc"] = Value::make_string("2.0");
    response["id"] = id;
    response["result"] = std::move(result);
    return Reply::ok(Value::make_object(std::move(response)).dump());
}

Reply Dispatcher::rpc_error(int code, const std::string& message, const Value& id) const {
    return Reply::ok(error_object(code, message, id).dump());
}

Reply Dispatcher::http_error(int status, int code, const std::string& message, const Value& id) const {
    Reply reply;
    reply.status = status;
    reply.body = error_object(code, message, id).dump();
    return reply;
}

Reply Dispatcher::handle(std::string_view message, Context ctx) {
    const Value null_id = Value::make_null();

    Value request;
    std::string parse_error_text;
    if (!Value::try_parse(message, request, parse_error_text)) {
        log_line("parse error: " + parse_error_text);
        return http_error(400, error_code::kParseError, "Parse error: " + parse_error_text, null_id);
    }

    // JSON-RPC batching was deprecated in protocol version 2025-03-26.
    if (request.is_array()) {
        return http_error(400, error_code::kInvalidRequest,
                          "JSON-RPC batching is not supported; send one message per request", null_id);
    }

    if (!request.is_object()) {
        return http_error(400, error_code::kInvalidRequest, "JSON-RPC message must be an object", null_id);
    }

    const Value* jsonrpc = request.find("jsonrpc");
    if (!jsonrpc || !jsonrpc->is_string() || jsonrpc->as_string() != "2.0") {
        return http_error(400, error_code::kInvalidRequest,
                          "Unsupported JSON-RPC version, expected \"2.0\"", null_id);
    }

    const Value* method = request.find("method");
    if (!method || !method->is_string()) {
        return http_error(400, error_code::kInvalidRequest, "Missing or non-string \"method\"", null_id);
    }

    const Value* id = request.find("id");
    const bool has_id = id && !id->is_null();
    const Value id_value = has_id ? *id : Value::make_null();

    if (verbose()) {
        std::ostringstream os;
        os << (has_id ? "request " : "notify  ") << method->as_string();
        if (has_id) os << " id=" << id_value.dump();
        if (!ctx.session_id.empty()) os << " session=" << ctx.session_id;
        if (!ctx.peer.empty()) os << " from " << ctx.peer;
        log_debug(os.str());
    }

    return dispatch(request, id_value, has_id, ctx);
}

Reply Dispatcher::dispatch(const Value& request, const Value& id, bool has_id, Context& ctx) {
    const std::string method = request.find("method")->as_string();
    const Value* params = request.find("params");
    if (params && !params->is_object() && !params->is_null()) {
        return rpc_error(error_code::kInvalidParams, "\"params\" must be an object", id);
    }
    const Value empty_params = Value::make_object();
    const Value& p = params ? *params : empty_params;

    // Notifications never get a reply body; transports send 202 Accepted.
    if (!has_id) {
        if (method == "notifications/initialized") {
            std::lock_guard<std::mutex> lock(mutex_);
            const auto it = sessions_.find(ctx.session_id);
            if (it != sessions_.end()) it->second.initialized = true;
            log_debug("client finished the handshake");
        } else if (method != "notifications/cancelled" && method != "notifications/progress" &&
                   method != "notifications/roots/list_changed") {
            log_line("ignoring unknown notification: " + method);
        }
        return Reply::accepted();
    }

    if (method == "initialize") return on_initialize(p, id, ctx);

    // Session handling. stdio is a single implicit session per process, so only
    // HTTP has anything to check. Under Lenient a request that carries no (or an
    // unrecognised) Mcp-Session-Id is served against an implicit anonymous
    // session rather than rejected - the spec leaves session ids optional, and a
    // client that cannot retain the header is still a legal caller. Strict
    // restores the lock-down; Stateless skips the whole notion.
    if (ctx.transport == "http" && session_policy_ != SessionPolicy::Stateless) {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto it = sessions_.find(ctx.session_id);
        const bool known = !ctx.session_id.empty() && it != sessions_.end();

        if (!known) {
            if (session_policy_ == SessionPolicy::Strict) {
                return http_error(404, error_code::kNotInitialized,
                                  "Unknown or missing Mcp-Session-Id; POST an initialize first", id);
            }
            log_debug("serving " + method + " without a known session (policy: " +
                      to_string(session_policy_) + ")");
        } else {
            ctx.client_name = it->second.client_name;
            ctx.client_version = it->second.client_version;
            // Demanding the handshake *notification* before any request is the
            // strictest reading of the spec, and it is the thing that trips
            // clients which fire tools/list immediately after initialize.
            if (!it->second.initialized && method != "ping" && session_policy_ == SessionPolicy::Strict) {
                return rpc_error(error_code::kInternalError,
                                 "Session not initialized; send notifications/initialized after initialize", id);
            }
        }
    }

    if (method == "ping") return make_result(Value::make_object(), id);
    if (method == "tools/list") return on_tools_list(p, id);
    if (method == "tools/call") return on_tools_call(p, id, ctx);
    if (method == "resources/list" || method == "prompts/list") {
        // Not advertised in capabilities, so a well-behaved client will not ask.
        // Answering explicitly is friendlier than a bare -32601.
        return make_result(method == "resources/list" ? Value::make_object(Object{{"resources", Value::make_array()}})
                                                      : Value::make_object(Object{{"prompts", Value::make_array()}}),
                           id);
    }

    return rpc_error(error_code::kMethodNotFound, "Method not found: " + method, id);
}

Reply Dispatcher::on_initialize(const Value& params, const Value& id, Context& ctx) {
    const Value* requested = params.find("protocolVersion");
    if (!requested || !requested->is_string()) {
        return rpc_error(error_code::kInvalidParams, "initialize requires params.protocolVersion (string)", id);
    }

    const std::string& version =
        std::find(supported_protocol_versions().begin(), supported_protocol_versions().end(),
                  requested->as_string()) != supported_protocol_versions().end()
            ? requested->as_string()
            : latest_protocol_version();

    // A stateless server hands out no id at all, which is the most compatible
    // mode there is: a client cannot misplace a header it never received.
    const bool issue_session = session_policy_ != SessionPolicy::Stateless;

    Session session;
    if (issue_session) session.id = ctx.transport == "http" ? new_session_id() : "stdio";
    session.protocol_version = version;
    if (const Value* info = params.find("clientInfo")) {
        if (const Value* name = info->find("name"); name && name->is_string()) session.client_name = name->as_string();
        if (const Value* v = info->find("version"); v && v->is_string()) session.client_version = v->as_string();
    }
    session.initialized = false;

    if (issue_session) {
        std::lock_guard<std::mutex> lock(mutex_);
        sessions_[session.id] = session;
    }

    ctx.session_id = session.id;
    ctx.client_name = session.client_name;
    ctx.client_version = session.client_version;

    Object tools_capability;
    tools_capability["listChanged"] = Value::make_bool(false);

    Object capabilities;
    capabilities["tools"] = Value::make_object(std::move(tools_capability));

    Object server_info;
    server_info["name"] = Value::make_string(name_);
    server_info["title"] = Value::make_string(name_);
    server_info["version"] = Value::make_string(version_);

    Object result;
    result["protocolVersion"] = Value::make_string(version);
    result["capabilities"] = Value::make_object(std::move(capabilities));
    result["serverInfo"] = Value::make_object(std::move(server_info));
    result["instructions"] = Value::make_string(
        "A demonstration MCP server. Call the hello tool to greet someone, echo to round-trip "
        "text, or utc_now for the current time.");

    Reply reply = make_result(Value::make_object(std::move(result)), id);
    reply.session_id = session.id;
    log_line("initialized: client=" + (session.client_name.empty() ? std::string("<unknown>") : session.client_name) +
             " protocol=" + version + " transport=" + ctx.transport +
             " session=" + (session.id.empty() ? std::string("<none, stateless>") : session.id));
    return reply;
}

bool parse_session_policy(const std::string& text, SessionPolicy& out) {
    std::string lowered;
    lowered.reserve(text.size());
    for (const char c : text) lowered.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));

    if (lowered == "lenient" || lowered == "loose" || lowered == "permissive") {
        out = SessionPolicy::Lenient;
        return true;
    }
    if (lowered == "strict" || lowered == "required" || lowered == "enforce") {
        out = SessionPolicy::Strict;
        return true;
    }
    if (lowered == "stateless" || lowered == "none" || lowered == "off") {
        out = SessionPolicy::Stateless;
        return true;
    }
    return false;
}

Reply Dispatcher::on_tools_list(const Value& /*params*/, const Value& id) const {
    Array tools;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        for (const auto& tool : tools_) {
            Object entry;
            entry["name"] = Value::make_string(tool.name);
            entry["description"] = Value::make_string(tool.description);
            entry["inputSchema"] = tool.input_schema;
            if (!tool.annotations.is_null()) entry["annotations"] = tool.annotations;
            tools.push_back(Value::make_object(std::move(entry)));
        }
    }
    Object result;
    result["tools"] = Value::make_array(std::move(tools));
    return make_result(Value::make_object(std::move(result)), id);
}

Reply Dispatcher::on_tools_call(const Value& params, const Value& id, Context& ctx) const {
    const Value* name = params.find("name");
    if (!name || !name->is_string()) {
        return rpc_error(error_code::kInvalidParams, "tools/call requires params.name (string)", id);
    }

    const Value arguments = Value::make_object();
    const Value* provided = params.find("arguments");
    if (provided && !provided->is_object()) {
        return rpc_error(error_code::kInvalidParams, "\"arguments\" must be an object", id);
    }
    const Value& args = provided ? *provided : arguments;

    const Tool* tool = nullptr;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto it = std::find_if(tools_.begin(), tools_.end(),
                                     [&](const Tool& t) { return t.name == name->as_string(); });
        if (it != tools_.end()) tool = &*it;
    }

    if (!tool) {
        // A missing tool is a protocol-level bad request, not a tool failure.
        return rpc_error(error_code::kInvalidParams, "Unknown tool: " + name->as_string(), id);
    }

    // A throwing handler is a tool execution failure, which the spec wants
    // reported inside the result as isError rather than as a JSON-RPC error.
    try {
        ToolResult result = tool->handler(args, ctx);
        Object body;
        body["content"] = Value::make_array(std::move(result.content));
        body["isError"] = Value::make_bool(result.is_error);
        return make_result(Value::make_object(std::move(body)), id);
    } catch (const std::exception& e) {
        log_line("tool '" + name->as_string() + "' threw: " + e.what());
        Object body;
        body["content"] = Value::make_array();
        Object block;
        block["type"] = Value::make_string("text");
        block["text"] = Value::make_string(std::string("Tool failed: ") + e.what());
        body["content"].as_array().push_back(Value::make_object(std::move(block)));
        body["isError"] = Value::make_bool(true);
        return make_result(Value::make_object(std::move(body)), id);
    }
}

// ---------------------------------------------------------------------------
// stdio transport
// ---------------------------------------------------------------------------

int serve_stdio(Dispatcher& dispatcher) {
    log_line("listening on stdio (protocol " + latest_protocol_version() + ", " +
             std::to_string(dispatcher.tool_count()) + " tools)");

    Context ctx;
    ctx.transport = "stdio";
    ctx.peer = "-";

    std::string line;
    std::size_t handled = 0;
    while (std::getline(std::cin, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();   // tolerate CRLF
        if (line.empty()) continue;
        Reply reply = dispatcher.handle(line, ctx);
        if (!reply.has_body || reply.body.empty()) continue;         // notifications: no reply
        std::cout << reply.body << '\n' << std::flush;
        ++handled;
    }
    log_line("stdin closed after " + std::to_string(handled) + " replies");
    return 0;
}

}  // namespace ggmcp
