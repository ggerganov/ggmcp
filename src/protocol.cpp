#include "ggmcp/protocol.hpp"

#include <fcntl.h>
#include <unistd.h>

#include <cerrno>
#include <string>

#include "ggmcp/config.hpp"
#include "ggmcp/session.hpp"
#include "ggmcp/tools.hpp"
#include "ggmcp/version.hpp"

namespace ggmcp {

const char* const kProtocolVersions[3] = {"2024-11-05", "2025-03-26", "2025-06-18"};
const char* const kLatestProtocolVersion = "2025-06-18";

bool knows_protocol_version(const std::string& version) {
    for (const char* known : kProtocolVersions) {
        if (version == known) return true;
    }
    return false;
}

Json rpc_result(const Json& id, const Json& result) {
    Json out;
    out["id"] = id;
    out["jsonrpc"] = "2.0";
    out["result"] = result;
    return out;
}

Json rpc_error(const Json& id, int code, const std::string& message, const Json& data) {
    Json error;
    error["code"] = code;
    error["message"] = message;
    if (!data.is_null()) error["data"] = data;

    Json out;
    out["error"] = error;
    out["id"] = id;
    out["jsonrpc"] = "2.0";
    return out;
}

Json protocol_fault(const std::string& code, const std::string& message) {
    Json out;
    out["error"] = code;
    out["message"] = message;
    return out;
}

Json routing_fault(int status, const std::string& message) {
    Json out;
    out["error"] = status;
    out["message"] = message;
    return out;
}

namespace {

Outcome fault(int status, const std::string& code, const std::string& message) {
    Outcome out;
    out.status = status;
    out.has_body = true;
    out.body = protocol_fault(code, message);
    return out;
}

Outcome reply(const Json& envelope) {
    Outcome out;
    out.status = 200;
    out.body = envelope;
    return out;
}

Outcome accepted() {
    Outcome out;
    out.status = 202;
    out.has_body = false;
    return out;
}

std::string text_of(const Json& object, const char* key) {
    if (!object.is_object()) return std::string();
    const auto it = object.find(key);
    if (it == object.end() || !it->is_string()) return std::string();
    return it->get<std::string>();
}

}  // namespace

Core::Core(const Config& config, SessionStore& sessions, const ToolRegistry& tools)
    : config_(&config), sessions_(&sessions), tools_(&tools) {
    if (::pipe(wakeup_) != 0) {
        wakeup_[0] = wakeup_[1] = -1;
        return;
    }
    for (int fd : wakeup_) {
        const int flags = ::fcntl(fd, F_GETFL, 0);
        if (flags >= 0) ::fcntl(fd, F_SETFL, flags | O_NONBLOCK);
    }
}

Core::~Core() {
    for (int fd : wakeup_) {
        if (fd >= 0) ::close(fd);
    }
}

void Core::stop() {
    stopping_.store(true);
    if (wakeup_[1] >= 0) {
        const char poke = 'x';
        const ssize_t ignored = ::write(wakeup_[1], &poke, 1);
        (void)ignored;
    }
}

Json Core::server_info() const {
    Json info;
    info["name"] = config_->name;
    info["title"] = config_->server_title();
    info["version"] = kVersion;
    return info;
}

Json Core::health() const {
    Json out;
    out["alias"] = config_->alias;
    out["auth"] = config_->require_token ? "token" : "off";
    out["authorization"] = config_->require_token ? "bearer token" : "off";
    out["confined"] = config_->confined();
    out["endpoint"] = config_->endpoint;
    out["policy"] = to_string(config_->session_policy);
    out["root"] = config_->fs_root;
    out["sessions"] = sessions_->size();
    out["status"] = "ok";
    out["title"] = config_->server_title();
    out["tools"] = tools_->names();
    out["transport"] = to_string(config_->transport);
    out["url"] = config_->url();
    out["version"] = kVersion;
    return out;
}

Outcome Core::gate(const std::string& method, const RequestEnvelope& env) const {
    Outcome pass;
    pass.status = 0;

    if (!env.protocol_version.empty() && !knows_protocol_version(env.protocol_version)) {
        return fault(400, "unsupported_protocol_version",
                     "'" + env.protocol_version +
                         "' is not a protocol version this server speaks; this server speaks " +
                         kProtocolVersions[0] + ", " + kProtocolVersions[1] + " and " +
                         kLatestProtocolVersion);
    }

    if (config_->session_policy == SessionPolicy::Stateless || env.transport == "stdio") {
        return pass;  // no session handling at all
    }

    if (method == "initialize") {
        if (!env.session_id.empty()) {
            return fault(400, "unexpected_session_id",
                         "initialize does not take an Mcp-Session-Id: there is no re-handshake, "
                         "close the session with DELETE and start a new one");
        }
        return pass;
    }

    if (env.session_id.empty()) {
        if (config_->session_policy == SessionPolicy::Strict) {
            return fault(400, "session_required",
                         "this server serves strict sessions: initialize first, then send the "
                         "Mcp-Session-Id it issues");
        }
        return pass;  // lenient: a request without a session is served as it arrives
    }

    Session session;
    if (!sessions_->get(env.session_id, &session)) {
        return fault(404, "session_not_found",
                     "no session '" + env.session_id + "' is open on this server");
    }
    // Lenient serves a session the moment initialize opens it; strict makes the
    // client finish the handshake first.
    const bool strict = config_->session_policy == SessionPolicy::Strict;
    if (strict && !session.initialized && method != "notifications/initialized") {
        return fault(400, "session_not_initialized",
                     "session '" + env.session_id +
                         "' has not finished the handshake: send notifications/initialized");
    }
    return pass;
}

Outcome Core::dispatch(const std::string& method, const Json& id, const Json& params,
                       const RequestEnvelope& env, std::string* issued) const {
    if (method == "initialize") {
        const std::string requested = text_of(params, "protocolVersion");
        const std::string negotiated =
            knows_protocol_version(requested) ? requested : kLatestProtocolVersion;

        Json capabilities;
        capabilities["tools"] = Json::object({{"listChanged", false}});

        Json result;
        result["capabilities"] = capabilities;
        result["protocolVersion"] = negotiated;
        result["serverInfo"] = server_info();
        if (!config_->instructions.empty()) result["instructions"] = config_->instructions;

        if (env.transport != "stdio" && config_->session_policy != SessionPolicy::Stateless) {
            Session session;
            session.protocol_version = negotiated;
            session.transport = env.transport;
            session.address = env.address;
            if (params.is_object() && params.contains("clientInfo")) {
                session.client_name = text_of(params["clientInfo"], "name");
                session.client_version = text_of(params["clientInfo"], "version");
            }
            session.initialized = false;
            *issued = sessions_->add(std::move(session));
        }
        return reply(rpc_result(id, result));
    }

    if (method == "notifications/initialized") {
        if (!env.session_id.empty()) sessions_->markInitialized(env.session_id);
        return accepted();
    }

    if (method == "ping") return reply(rpc_result(id, Json::object()));

    if (method == "tools/list") {
        Json result;
        result["tools"] = tools_->published();
        return reply(rpc_result(id, result));
    }

    if (method == "tools/call") {
        Json data;
        data["available"] = tools_->names();
        data["prefix"] = config_->alias.empty() ? "(none)" : config_->alias;

        if (!params.is_object()) {
            return reply(rpc_error(id, kRpcInvalidParams,
                                   "tools/call wants a params object holding a string \"name\"",
                                   data));
        }
        const std::string name = text_of(params, "name");
        if (name.empty()) {
            return reply(rpc_error(id, kRpcInvalidParams,
                                   "tools/call wants params.\"name\" to name a published tool",
                                   data));
        }
        if (tools_->find(name) == nullptr) {
            Json unknown;
            unknown["available"] = data["available"];
            unknown["prefix"] = data["prefix"];
            unknown["tool"] = name;
            return reply(rpc_error(
                id, kRpcInvalidParams,
                "unknown tool '" + name + "'; this server publishes " +
                    (config_->alias.empty() ? std::string("the bare names")
                                            : "tools under the '" + config_->alias + "-' prefix"),
                unknown));
        }

        Json arguments = Json::object();
        const auto arguments_it = params.find("arguments");
        if (arguments_it != params.end() && !arguments_it->is_null()) {
            if (!arguments_it->is_object()) {
                return reply(rpc_error(id, kRpcInvalidParams,
                                       "tools/call wants params.\"arguments\" to be an object",
                                       data));
            }
            arguments = *arguments_it;
        }

        ToolContext ctx;
        ctx.config = config_;
        ctx.transport = env.transport;
        ctx.address = env.address;
        ctx.session_id = env.session_id;

        const ToolOutcome outcome = tools_->call(name, arguments, ctx);

        Json block;
        block["text"] = outcome.text;
        block["type"] = "text";
        Json result;
        result["content"] = Json::array({block});
        result["isError"] = !outcome.ok;
        return reply(rpc_result(id, result));
    }

    if (method == "resources/list") {
        Json result;
        result["resources"] = Json::array();
        return reply(rpc_result(id, result));
    }

    if (method == "prompts/list") {
        Json result;
        result["prompts"] = Json::array();
        return reply(rpc_result(id, result));
    }

    Json data;
    data["known"] = Json::array({"initialize", "ping",                "tools/list",
                                 "tools/call", "resources/list",      "prompts/list",
                                 "notifications/initialized"});
    data["method"] = method;
    return reply(rpc_error(id, kRpcMethodNotFound, "method not found: " + method, data));
}

Outcome Core::handle_body(const std::string& body, const RequestEnvelope& env) const {
    Json request;
    bool parsed = false;
    try {
        request = Json::parse(body);
        parsed = true;
    } catch (const Json::exception&) {
        parsed = false;
    }

    std::string method;
    Json id;
    Json params;
    bool has_id = false;
    bool batched = false;
    bool well_formed = false;

    if (parsed) {
        if (request.is_array()) {
            batched = true;
        } else if (request.is_object()) {
            well_formed = request.contains("method") && request["method"].is_string();
            if (well_formed) method = request["method"].get<std::string>();
            const auto id_it = request.find("id");
            has_id = id_it != request.end() && !id_it->is_null();
            if (has_id) id = *id_it;
            const auto params_it = request.find("params");
            if (params_it != request.end()) params = *params_it;
        }
    }

    std::string issued;
    const Outcome gated = gate(method, env);
    if (gated.status != 0) return gated;

    if (!parsed) {
        return reply(rpc_error(id, kRpcParseError, "the request body is empty or is not JSON"));
    }
    if (batched) {
        Json data;
        data["supported"] = Json::array({"one request per call"});
        return reply(rpc_error(id, kRpcInvalidRequest,
                               "JSON-RPC batching is not supported: send one request per call",
                               data));
    }
    if (!well_formed) {
        return reply(rpc_error(id, kRpcInvalidRequest,
                               "a JSON-RPC request needs a string \"method\""));
    }

    Outcome outcome = dispatch(method, id, params, env, &issued);
    outcome.session_id = issued;
    if (!has_id) {
        // A notification is answered with 202 and no body, whatever it did on the way.
        outcome.status = 202;
        outcome.has_body = false;
        outcome.body = Json();
    }
    return outcome;
}

Outcome Core::close_session(const std::string& session_id) const {
    if (session_id.empty()) {
        return fault(400, "bad_request", "DELETE wants the session to close in Mcp-Session-Id");
    }
    if (config_->session_policy == SessionPolicy::Stateless) {
        return fault(404, "session_not_found",
                     "this server serves stateless sessions and keeps none to close");
    }
    if (!sessions_->erase(session_id)) {
        return fault(404, "session_not_found",
                     "no session '" + session_id + "' is open on this server");
    }
    Outcome out;
    out.status = 200;
    Json body;
    body["closed"] = true;
    body["session"] = session_id;
    out.body = body;
    return out;
}

}  // namespace ggmcp
