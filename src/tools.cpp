// ggmcp - a tiny Model Context Protocol server built on vendored single-header libraries.
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
    const Value* value = find(args, key);
    if (!value || value->is_null()) return true;
    if (!value->is_string()) {
        error = std::string("\"") + key + "\" must be a string";
        return false;
    }
    out = value->get<std::string>();
    return true;
}

bool optional_bool(const Value& args, const char* key, bool& out, std::string& error) {
    const Value* value = find(args, key);
    if (!value || value->is_null()) return true;
    if (!value->is_boolean()) {
        error = std::string("\"") + key + "\" must be a boolean";
        return false;
    }
    out = value->get<bool>();
    return true;
}

/// Convenience for a tool that answers with one JSON document.
ToolResult as_json(Object payload) { return ToolResult::json(dump(Value(std::move(payload)))); }

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
        tool.annotations = tool_annotations("Say hello", /*read_only=*/true, /*destructive=*/false,
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
        tool.annotations = tool_annotations("Echo text", /*read_only=*/true, /*destructive=*/false,
                                      /*idempotent=*/true);
        tool.handler = [](const Value& args, const Context&) -> ToolResult {
            const Value* message = find(args, "message");
            if (!message) return ToolResult::error("\"message\" is required");
            if (!message->is_string()) return ToolResult::error("\"message\" must be a string");
            if (message->get<std::string>().empty()) return ToolResult::error("\"message\" must not be empty");
            return ToolResult::text(message->get<std::string>());
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
        tool.annotations = tool_annotations("Current UTC time", /*read_only=*/true, /*destructive=*/false,
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
            payload["iso8601"] = Value(iso);
            payload["weekday"] = Value(weekday);
            payload["epochMillis"] = Value(static_cast<std::int64_t>(millis));
            payload["timezone"] = Value("UTC");
            return as_json(std::move(payload));
        };
        dispatcher.add_tool(std::move(tool));
    }

    // --- whoami ----------------------------------------------------------
    {
        Tool tool;
        tool.name = "whoami";
        tool.description = "Describes the current MCP session: transport, session id, peer address, "
                           "credential source and the clientInfo the caller sent during initialize. "
                           "Handy for debugging a client connection.";
        tool.input_schema = schema::object({}, {});
        tool.annotations = tool_annotations("Describe this session", /*read_only=*/true, /*destructive=*/false,
                                      /*idempotent=*/true);
        tool.handler = [](const Value&, const Context& ctx) -> ToolResult {
            Object payload;
            payload["transport"] = Value(ctx.transport);
            // "(implicit)" means the caller sent no usable Mcp-Session-Id and was
            // served against an implicit session - legal under the default policy.
            payload["sessionId"] = Value(ctx.session_id.empty() ? std::string("(implicit)")
                                                                              : ctx.session_id);
            payload["peer"] = Value(ctx.peer);
            // Which credential source authenticated this call ("env:GGMCP_TOKEN",
            // "tokens.txt:2", or "anonymous"). The secret itself is never in reach
            // of a tool, so it cannot be echoed back to the caller that presented it.
            payload["authorization"] = Value(ctx.auth.empty() ? std::string("anonymous")
                                                                           : ctx.auth);
            payload["clientName"] = Value(ctx.client_name);
            payload["clientVersion"] = Value(ctx.client_version);
            payload["server"] = Value(std::string(kDefaultServerName) + "/" +
                                                   kDefaultServerVersion);
            return as_json(std::move(payload));
        };
        dispatcher.add_tool(std::move(tool));
    }
}

}  // namespace ggmcp
