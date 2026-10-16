#include "gg/mcp/protocol.hpp"

#include <algorithm>
#include <cctype>
#include <stdexcept>

namespace gg::mcp {
namespace protocol {
namespace {

bool contains(const std::vector<std::string>& values, const std::string& value) {
    return std::find(values.begin(), values.end(), value) != values.end();
}

} // namespace

std::vector<std::string> supportedVersions() {
    return {kVersion20241105, kVersion20250326, kVersion20250618};
}

std::string newestVersion() {
    return kVersion20250618;
}

bool isSupported(const std::string& version) {
    return contains(supportedVersions(), version);
}

std::string negotiate(const std::string& requested) {
    return isSupported(requested) ? requested : newestVersion();
}

Json error(const Json& id, int code, const std::string& message, const Json& data) {
    Json error_ = Json::object({{"code", static_cast<long long>(code)}, {"message", message}});
    if (!data.isNull()) {
        error_.set("data", data);
    }
    Json response = Json::object();
    response.set("jsonrpc", "2.0");
    response.set("id", id);
    response.set("error", error_);
    return response;
}

Json result(const Json& id, const Json& payload) {
    Json response = Json::object();
    response.set("jsonrpc", "2.0");
    response.set("id", id);
    response.set("result", payload);
    return response;
}

Json transportFault(const std::string& code, const std::string& message) {
    return Json::object({{"error", code}, {"message", message}});
}

} // namespace protocol

Outbound Outbound::jsonRpc(int status, Json payload) {
    Outbound outbound;
    outbound.status = status;
    outbound.hasBody = true;
    outbound.payload = std::move(payload);
    return outbound;
}

Outbound Outbound::fault(int status, const std::string& code, const std::string& message) {
    Outbound outbound;
    outbound.status = status;
    outbound.hasBody = true;
    outbound.payload = protocol::transportFault(code, message);
    return outbound;
}

Outbound Outbound::accepted() {
    Outbound outbound;
    outbound.status = 202;
    outbound.hasBody = false;
    outbound.contentType.clear();
    return outbound;
}

Protocol::Protocol(Options options, ToolRegistry& tools, SessionStore& sessions)
    : options_(std::move(options)), tools_(tools), sessions_(sessions) {}

int Protocol::destroy(const std::string& sessionId) {
    if (options_.sessionPolicy == SessionPolicy::Stateless) {
        return 405;
    }
    if (sessionId.empty()) {
        return 400;
    }
    return sessions_.remove(sessionId) ? 200 : 404;
}

ToolContext Protocol::toolContext(const Inbound& inbound, const Session* session) const {
    ToolContext context;
    context.transport = inbound.transport;
    context.clientAddress = inbound.clientAddress;
    context.root = options_.root();
    context.confineToRoot = !options_.allowOutsideRoot;
    context.session = session ? describeSession(*session)
                              : describeMissingSession(options_.sessionPolicy, inbound.transport);
    return context;
}

Outbound Protocol::handle(const Inbound& inbound) {
    if (!inbound.contentType.empty() && inbound.contentType.find("json") == std::string::npos) {
        return Outbound::fault(415, "unsupported_media_type",
                               "the request body must be application/json");
    }
    if (!inbound.accept.empty() && inbound.accept.find("json") == std::string::npos &&
        inbound.accept.find("*/*") == std::string::npos) {
        return Outbound::fault(406, "not_acceptable",
                               "this server only answers with application/json");
    }

    Json message;
    std::string parseError;
    if (!Json::tryParse(inbound.body, message, &parseError)) {
        return Outbound::jsonRpc(200,
                                 protocol::error(Json(), protocol::error_code::kParseError,
                                                 "parse error: " + parseError));
    }

    const Json id = message.contains("id") ? message.at("id") : Json();

    if (!message.isObject() || !message.at("method").isString() ||
        (message.contains("jsonrpc") && message.at("jsonrpc").asString() != "2.0")) {
        return Outbound::jsonRpc(200, protocol::error(id, protocol::error_code::kInvalidRequest,
                                                      "invalid request"));
    }

    const std::string method = message.at("method").asString();

    Session session;
    bool haveSession = false;
    if (options_.sessionPolicy != SessionPolicy::Stateless && !inbound.sessionId.empty()) {
        if (!sessions_.get(inbound.sessionId, session)) {
            return Outbound::fault(404, "session_not_found",
                                   std::string("no session is known for this '") +
                                       protocol::kSessionIdHeader + "'");
        }
        haveSession = true;
    }

    if (method != "initialize") {
        if (options_.sessionPolicy == SessionPolicy::Strict) {
            if (!haveSession) {
                return Outbound::fault(400, "session_required",
                                       "send a session id issued by initialize in "
                                       "the Mcp-Session-Id header");
            }
            if (!session.initialized && method != "notifications/initialized") {
                return Outbound::fault(400, "session_not_initialized",
                                       "finish the handshake with notifications/initialized");
            }
        }
        if (!inbound.protocolVersionHeader.empty() &&
            !protocol::isSupported(inbound.protocolVersionHeader)) {
            return Outbound::fault(400, "unsupported_protocol_version",
                                   "this server does not speak protocol version '" +
                                       inbound.protocolVersionHeader + "'");
        }
    } else if (haveSession) {
        return Outbound::fault(400, "unexpected_session_id",
                               "initialize must be sent without a session id");
    }

    if (haveSession) {
        sessions_.update(inbound.sessionId, [](Session& live) { ++live.requests; });
        session.requests += 1;
    }

    return dispatch(message, id, method, inbound, haveSession ? &session : nullptr);
}

Outbound Protocol::dispatch(const Json& message, const Json& id, const std::string& method,
                            const Inbound& inbound, const Session* session) {
    const Json params = message.at("params");

    if (method == "initialize") {
        return handleInitialize(id, params, inbound);
    }

    if (method == "notifications/initialized") {
        if (session != nullptr) {
            sessions_.update(inbound.sessionId, [](Session& live) { live.initialized = true; });
        }
        return Outbound::accepted();
    }

    if (method == "ping") {
        return Outbound::jsonRpc(200, protocol::result(id, Json::object()));
    }

    if (method == "tools/list") {
        return Outbound::jsonRpc(200, protocol::result(id, tools_.list()));
    }

    if (method == "tools/call") {
        return handleToolsCall(id, params, inbound, session);
    }

    if (method == "resources/list") {
        return Outbound::jsonRpc(200, protocol::result(id, Json::object({{"resources", Json::array()}})));
    }

    if (method == "prompts/list") {
        return Outbound::jsonRpc(200, protocol::result(id, Json::object({{"prompts", Json::array()}})));
    }

    if (method.rfind("notifications/", 0) == 0) {
        return Outbound::accepted();
    }

    return Outbound::jsonRpc(
        200, protocol::error(id, protocol::error_code::kMethodNotFound,
                             "method not found: " + method,
                             Json::object({{"method", method}})));
}

Outbound Protocol::handleInitialize(const Json& id, const Json& params, const Inbound& inbound) {
    const std::string requested = params.getString("protocolVersion");
    const Json clientInfo = params.at("clientInfo");

    Session session;
    session.protocolVersion = protocol::negotiate(requested);
    session.clientName = clientInfo.getString("name");
    session.clientVersion = clientInfo.getString("version");
    session.transport = inbound.transport;
    session.origin = inbound.origin;
    session.clientAddress = inbound.clientAddress;
    session.userAgent = inbound.userAgent;
    session.initialized = false;
    session.requests = 1;

    Outbound outbound;
    if (options_.sessionPolicy != SessionPolicy::Stateless) {
        outbound.sessionId = sessions_.create(session);
    }

    Json capabilities = Json::object({{"tools", Json::object({{"listChanged", false}})}});
    Json serverInfo = Json::object({{"name", options_.serverName},
                                    {"title", options_.serverTitle},
                                    {"version", options_.serverVersion}});
    Json payload = Json::object({{"protocolVersion", session.protocolVersion},
                                 {"capabilities", capabilities},
                                 {"serverInfo", serverInfo},
                                 {"instructions", options_.instructions}});

    outbound.payload = protocol::result(id, payload);
    outbound.method = "initialize";
    return outbound;
}

Outbound Protocol::handleToolsCall(const Json& id, const Json& params, const Inbound& inbound,
                                   const Session* session) {
    if (!params.isObject() || !params.hasText("name")) {
        return Outbound::jsonRpc(200,
                                 protocol::error(id, protocol::error_code::kInvalidParams,
                                                 "tools/call needs a string 'name' in params"));
    }

    const std::string name = params.at("name").asString();
    const Tool* tool = tools_.find(name);
    if (tool == nullptr) {
        const std::string bare = tools_.bare(name);
        std::string hint = "unknown tool '" + name + "'";
        if (!options_.alias.empty() && name == bare) {
            hint += ", this server publishes tools as '" + options_.alias + "-<tool>'";
        }
        if (!options_.fileTools && (bare == "read" || bare == "write" || bare == "edit")) {
            hint += ", the file tools are turned off by --no-file-tools";
        }
        if (!options_.bashTool && bare == "bash") {
            hint += ", the bash tool is turned off by --no-bash";
        }
        Json available = Json::array();
        for (const auto& toolName : tools_.names()) {
            available.push_back(toolName);
        }
        return Outbound::jsonRpc(200,
                                 protocol::error(id, protocol::error_code::kInvalidParams, hint,
                                                 Json::object({{"available", available}})));
    }

    const Json arguments = params.at("arguments");
    if (!arguments.isNull() && !arguments.isObject()) {
        return Outbound::jsonRpc(
            200, protocol::error(id, protocol::error_code::kInvalidParams,
                                 "tools/call 'arguments' must be an object"));
    }

    try {
        const ToolResult result = tool->handler(toolContext(inbound, session), arguments);
        return Outbound::jsonRpc(200, protocol::result(id, result.toJson()));
    } catch (const std::exception& e) {
        return Outbound::jsonRpc(200, protocol::error(id, protocol::error_code::kInternalError,
                                                      "tool '" + name + "' failed: " + e.what()));
    }
}

std::string Protocol::handleLine(const std::string& line) {
    Inbound inbound;
    inbound.transport = "stdio";
    inbound.body = line;
    const Outbound outbound = handle(inbound);
    if (!outbound.hasBody) {
        return "";
    }
    return outbound.payload.dump();
}

} // namespace gg::mcp
