// ggmcp - a tiny Model Context Protocol server in dependency-free C++17.
//
// SPDX-License-Identifier: MIT
//
// tools.cpp - the four demo tools. Split out of main.cpp so the tests can
// register the same tools a real client sees.

#include "ggmcp/tools.hpp"

#include <chrono>
#include <cstdio>
#include <ctime>
#include <string>

namespace ggmcp {
namespace {

/// Uppercases ASCII letters and leaves everything else, including UTF-8 bytes,
/// alone. Enough for a demo; not a general Unicode case mapping.
std::string shout(std::string text) {
    for (char& c : text) {
        if (c >= 'a' && c <= 'z') c = static_cast<char>(c - 'a' + 'A');
    }
    return text;
}

/// Reads an optional string argument, rejecting the wrong type rather than
/// coercing it - a tool should tell the model what it got wrong.
bool optional_string(const Value& args, const char* key, std::string& out, std::string& error) {
    const Value* value = args.find(key);
    if (!value || value->is_null()) return true;
    if (!value->is_string()) {
        error = std::string("\"") + key + "\" must be a string";
        return false;
    }
    out = value->as_string();
    return true;
}

bool optional_bool(const Value& args, const char* key, bool& out, std::string& error) {
    const Value* value = args.find(key);
    if (!value || value->is_null()) return true;
    if (!value->is_bool()) {
        error = std::string("\"") + key + "\" must be a boolean";
        return false;
    }
    out = value->as_bool();
    return true;
}

Value annotations(const std::string& title, bool read_only, bool destructive, bool idempotent) {
    Object object;
    object["title"] = Value::make_string(title);
    object["readOnlyHint"] = Value::make_bool(read_only);
    object["destructiveHint"] = Value::make_bool(destructive);
    object["idempotentHint"] = Value::make_bool(idempotent);
    object["openWorldHint"] = Value::make_bool(false);
    return Value::make_object(std::move(object));
}

/// Convenience for a tool that answers with one JSON document.
ToolResult as_json(Object payload) { return ToolResult::json(Value::make_object(std::move(payload)).dump()); }

}  // namespace

void register_demo_tools(Dispatcher& dispatcher) {
    // --- hello -----------------------------------------------------------
    {
        Tool tool;
        tool.name = "hello";
        tool.description =
            "Greets someone. With no arguments it says hello to the world; pass a name to be "
            "specific. Read-only and side-effect free.";
        tool.input_schema = schema::object(
            {{"name", schema::string("Who to greet. Defaults to \"world\".")},
             {"loud", schema::boolean("Shout the greeting in upper case.")}},
            {});
        tool.annotations = annotations("Say hello", /*read_only=*/true, /*destructive=*/false,
                                      /*idempotent=*/true);
        tool.handler = [](const Value& args, const Context& ctx) -> ToolResult {
            std::string error;
            std::string who;
            bool loud = false;
            if (!optional_string(args, "name", who, error)) return ToolResult::error(error);
            if (!optional_bool(args, "loud", loud, error)) return ToolResult::error(error);
            if (who.empty()) who = "world";
            std::string text = "Hello, " + who + "!";
            if (loud) text = shout(text);
            text += " (served over " + ctx.transport + " by " + kDefaultServerName + "/" +
                    kDefaultServerVersion + ")";
            return ToolResult::text(std::move(text));
        };
        dispatcher.add_tool(std::move(tool));
    }

    // --- echo ------------------------------------------------------------
    {
        Tool tool;
        tool.name = "echo";
        tool.description = "Returns the given message back verbatim. Useful for checking that a "
                           "client round-trips arguments and content correctly.";
        tool.input_schema = schema::object({{"message", schema::string("Text to echo back unchanged.")}},
                                           {"message"});
        tool.annotations = annotations("Echo text", /*read_only=*/true, /*destructive=*/false,
                                      /*idempotent=*/true);
        tool.handler = [](const Value& args, const Context&) -> ToolResult {
            const Value* message = args.find("message");
            if (!message) return ToolResult::error("\"message\" is required");
            if (!message->is_string()) return ToolResult::error("\"message\" must be a string");
            if (message->as_string().empty()) return ToolResult::error("\"message\" must not be empty");
            return ToolResult::text(message->as_string());
        };
        dispatcher.add_tool(std::move(tool));
    }

    // --- utc_now ---------------------------------------------------------
    {
        Tool tool;
        tool.name = "utc_now";
        tool.description = "Reports the server's current wall-clock time in UTC as ISO-8601 plus "
                           "epoch milliseconds. Takes no arguments.";
        tool.input_schema = schema::object({}, {});
        tool.annotations = annotations("Current UTC time", /*read_only=*/true, /*destructive=*/false,
                                      /*idempotent=*/false);
        tool.handler = [](const Value&, const Context&) -> ToolResult {
            const auto now = std::chrono::system_clock::now();
            const auto millis =
                std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count();
            const std::time_t seconds =
                std::chrono::duration_cast<std::chrono::seconds>(now.time_since_epoch()).count();
            std::tm utc{};
            ::gmtime_r(&seconds, &utc);

            char iso[32] = {0};
            std::strftime(iso, sizeof(iso), "%Y-%m-%dT%H:%M:%SZ", &utc);
            char weekday[16] = {0};
            std::strftime(weekday, sizeof(weekday), "%A", &utc);

            Object payload;
            payload["iso8601"] = Value::make_string(iso);
            payload["weekday"] = Value::make_string(weekday);
            payload["epochMillis"] = Value::make_int(static_cast<std::int64_t>(millis));
            payload["timezone"] = Value::make_string("UTC");
            return as_json(std::move(payload));
        };
        dispatcher.add_tool(std::move(tool));
    }

    // --- whoami ----------------------------------------------------------
    {
        Tool tool;
        tool.name = "whoami";
        tool.description = "Describes the current MCP session: transport, session id, peer address "
                           "and the clientInfo the caller sent during initialize. Handy for "
                           "debugging a client connection.";
        tool.input_schema = schema::object({}, {});
        tool.annotations = annotations("Describe this session", /*read_only=*/true, /*destructive=*/false,
                                      /*idempotent=*/true);
        tool.handler = [](const Value&, const Context& ctx) -> ToolResult {
            Object payload;
            payload["transport"] = Value::make_string(ctx.transport);
            // "(implicit)" means the caller sent no usable Mcp-Session-Id and was
            // served against an implicit session - legal under the default policy.
            payload["sessionId"] = Value::make_string(ctx.session_id.empty() ? std::string("(implicit)")
                                                                              : ctx.session_id);
            payload["peer"] = Value::make_string(ctx.peer);
            payload["clientName"] = Value::make_string(ctx.client_name);
            payload["clientVersion"] = Value::make_string(ctx.client_version);
            payload["server"] = Value::make_string(std::string(kDefaultServerName) + "/" +
                                                   kDefaultServerVersion);
            return as_json(std::move(payload));
        };
        dispatcher.add_tool(std::move(tool));
    }
}

}  // namespace ggmcp
