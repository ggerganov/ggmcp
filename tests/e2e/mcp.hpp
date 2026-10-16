#pragma once

#include <sstream>
#include <string>

#include "harness.hpp"
#include "proc.hpp"

namespace e2e {

inline std::string str(long long value) { return std::to_string(value); }

inline void check_status(const Message& message, int want, const char* file, int line,
                         const char* what) {
    if (message.status == want) return;
    std::ostringstream why;
    why << what << " answered " << message.status << " " << message.reason
        << " with body: " << message.body;
    fail(file, line, why.str());
}

// A negative id means a notification: no id on the wire, and no answer.
inline Json envelope(const std::string& method, const Json& params, long long id) {
    Json out;
    if (id >= 0) out["id"] = id;
    out["jsonrpc"] = "2.0";
    out["method"] = method;
    if (!params.is_null()) out["params"] = params;
    return out;
}

inline Json init_params(const std::string& version = "2025-06-18") {
    Json client;
    client["name"] = "e2e";
    client["version"] = "0";
    Json params;
    params["capabilities"] = Json::object();
    params["clientInfo"] = client;
    params["protocolVersion"] = version;
    return params;
}

// One JSON-RPC message, as a POST: the shape every MCP client uses.
inline Call rpc(const std::string& endpoint, const Json& body, const std::string& session = "") {
    Call call = post(endpoint, body.dump());
    if (!session.empty()) call.with("Mcp-Session-Id", session);
    return call;
}

// The full handshake, returning the session id.
inline std::string initialize(Server& server, const std::string& endpoint = "/mcp",
                              const std::string& version = "2025-06-18") {
    Message opened = server.request(rpc(endpoint, envelope("initialize", init_params(version), 1)));
    if (opened.status != 200) {
        fail(__FILE__, __LINE__, "initialize answered " + std::to_string(opened.status) + ": " + opened.body);
    }
    const std::string session = opened.header("Mcp-Session-Id");
    if (session.empty()) fail(__FILE__, __LINE__, "initialize issued no session id: " + opened.body);
    server.request(rpc(endpoint, envelope("notifications/initialized", Json(), -1), session));
    return session;
}

// A tool call, with the alias this server publishes.
inline Json tool_call(const std::string& name, const Json& arguments) {
    Json params;
    params["arguments"] = arguments;
    params["name"] = name;
    return params;
}

inline std::string text_of(const Json& answer) {
    if (!answer.contains("result") || !answer["result"].contains("content") ||
        !answer["result"]["content"].is_array() || answer["result"]["content"].empty()) {
        throw Failure("no content block in " + answer.dump());
    }
    return answer["result"]["content"][0].value("text", std::string());
}

inline bool is_error(const Json& answer) {
    return answer.contains("result") && answer["result"].value("isError", false);
}

}  // namespace e2e

#define CHECK_STATUS(message, want) ::e2e::check_status((message), (want), __FILE__, __LINE__, #message)
