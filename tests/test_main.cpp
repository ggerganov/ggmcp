// ggmcp - a tiny Model Context Protocol server built on vendored single-header libraries.
//
// SPDX-License-Identifier: MIT
//
// test_main.cpp - in-process tests for the JSON layer and the MCP dispatcher.
// No sockets here; tests/http_roundtrip.py drives the HTTP transport.

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

#include "ggmcp/auth.hpp"
#include "ggmcp/json.hpp"
#include "ggmcp/mcp_http.hpp"
#include "ggmcp/protocol.hpp"
#include "ggmcp/tools.hpp"

// ggmcp::Value is an alias for nlohmann::json, so the JSON lookup helpers
// declared in namespace ggmcp are not reachable by ADL from this translation
// unit - an alias does not add its spelling namespace to the lookup.
using ggmcp::find;

namespace {

int g_checks = 0;
int g_failures = 0;
std::string g_group = "general";

void group(const std::string& name) { g_group = name; }

void check(bool condition, const std::string& what) {
    ++g_checks;
    if (condition) return;
    ++g_failures;
    std::fprintf(stderr, "FAIL [%s] %s\n", g_group.c_str(), what.c_str());
}

void check_eq(const std::string& got, const std::string& want, const std::string& what) {
    ++g_checks;
    if (got == want) return;
    ++g_failures;
    std::fprintf(stderr, "FAIL [%s] %s\n  got:  %s\n  want: %s\n", g_group.c_str(), what.c_str(),
                 got.c_str(), want.c_str());
}

bool contains(const std::string& haystack, const std::string& needle) {
    return haystack.find(needle) != std::string::npos;
}

void check_has(const std::string& haystack, const std::string& needle, const std::string& what) {
    check(contains(haystack, needle), what + " (expected to contain \"" + needle + "\")");
}

// ---------------------------------------------------------------------------
// JSON
// ---------------------------------------------------------------------------

void test_json_scalars() {
    group("json/scalars");

    check(ggmcp::parse("null").is_null(), "null parses");
    check(ggmcp::parse("true").get<bool>(), "true parses");
    check(!ggmcp::parse("false").get<bool>(), "false parses");
    check_eq(std::to_string(ggmcp::parse("42").get<std::int64_t>()), "42", "integer parses");
    check_eq(std::to_string(ggmcp::parse("-7").get<std::int64_t>()), "-7", "negative integer parses");
    check(ggmcp::parse("3.5").is_number_float(), "double parses");
    check(ggmcp::parse("1e3").is_number_float(), "exponent parses as double");
    check(ggmcp::parse("  \t\n 12 \r\n").is_number_integer(), "surrounding whitespace is skipped");

    // An integer too wide for int64 degrades to a double rather than wrapping.
    const ggmcp::Value huge = ggmcp::parse("99999999999999999999");
    check(huge.is_number_float(), "out-of-range integer falls back to double");
}

void test_json_strings() {
    group("json/strings");

    check_eq(ggmcp::parse("\"plain\"").get<std::string>(), "plain", "plain string");
    check_eq(ggmcp::parse("\"a\\nb\"").get<std::string>(), "a\nb", "escape n");
    check_eq(ggmcp::parse("\"a\\\"b\"").get<std::string>(), "a\"b", "escaped quote");
    check_eq(ggmcp::parse("\"a\\\\b\"").get<std::string>(), "a\\b", "escaped backslash");
    check_eq(ggmcp::parse("\"\\u0041\"").get<std::string>(), "A", "\\u escape");

    // U+1F600 GRINNING FACE, written as a surrogate pair, must become 4-byte UTF-8.
    const std::string emoji = ggmcp::parse("\"\\ud83d\\ude00\"").get<std::string>();
    const std::string want_emoji = "\xF0\x9F\x98\x80";
    check_eq(emoji, want_emoji, "\\uXXXX surrogate pair decodes to UTF-8");

    // Raw UTF-8 in must survive, and raw UTF-8 out must not be escaped away.
    const std::string text = "\"caf\xC3\xA9\"";
    check_eq(ggmcp::parse(text).get<std::string>(), "caf\xC3\xA9", "raw UTF-8 input decodes");
    check_eq(ggmcp::Value("caf\xC3\xA9").dump(), "\"caf\xC3\xA9\"",
             "raw UTF-8 output is not escaped");

    // ggmcp::dump() is lenient where Value::dump() throws: a tool that echoes
    // a byte which is not part of a character must still produce a body, so
    // the byte becomes U+FFFD instead of an exception and a 500.
    check_eq(ggmcp::dump(ggmcp::Value(std::string("a") + static_cast<char>(0xFF) + "z")),
             "\"a\xEF\xBF\xBDz\"", "invalid UTF-8 is replaced with U+FFFD on dump");

    // Round trip through dump/parse.
    const ggmcp::Value original = ggmcp::parse("[\"a\\tb\",1.5,{\"k\":[true,null]}]");
    const ggmcp::Value reparsed = ggmcp::parse(original.dump());
    check(reparsed.is_array() && reparsed.size() == 3, "array round trip");
    check_eq(reparsed.at(0).get<std::string>(), "a\tb", "nested string round trip");
    check(find(reparsed.at(2), "k")->at(1).is_null(), "nested null round trip");
}

void test_json_containers() {
    group("json/containers");

    const ggmcp::Value empty_array = ggmcp::parse("[]");
    check(empty_array.is_array() && empty_array.size() == 0, "empty array");

    const ggmcp::Value empty_object = ggmcp::parse("{}");
    check(empty_object.is_object() && empty_object.size() == 0, "empty object");

    const ggmcp::Value object = ggmcp::parse("{\"b\":1,\"a\":2}");
    check(object.is_object() && object.size() == 2, "object with two keys");
    check_eq(object.dump(), "{\"a\":2,\"b\":1}", "object keys serialize in sorted order");

    // Sorted keys make server responses byte-stable, which makes tests easy.
    check_eq(ggmcp::parse("{\"x\":\"\\u00e9\"}").dump(), "{\"x\":\"\xC3\xA9\"}", "dump emits UTF-8");

    // Control characters must be escaped on output to stay valid JSON.
    check_eq(ggmcp::Value(std::string("a") + static_cast<char>(0x01) + "b").dump(),
             "\"a\\u0001b\"", "control characters are escaped");
}

void test_json_errors() {
    group("json/errors");

    const std::vector<std::string> bad = {
        "", "nul", "{", "}", "[", "]", "{\"", "{\"a\":}", "{\"a\" 1}", "[1,]", "1 2", "\"unterminated",
        "0x10", ".5", "+1", "tru", "\"\\uZZZZ\"", "\"\\uD800\"", "\"\\uDC00\"", "\"\x01\"",
    };
    for (const auto& text : bad) {
        ggmcp::Value value;
        std::string error;
        const bool ok = ggmcp::try_parse(text, value, error);
        check(!ok, "rejects \"" + text + "\"");
    }

}

void test_json_depth_limit() {
    group("json/depth");
    ggmcp::Value value;
    std::string error;
    const std::string deep(200, '[');
    check(!ggmcp::try_parse(deep, value, error), "deeply nested arrays are rejected");
    check(contains(error, "deep"), "depth error mentions depth");
}

// ---------------------------------------------------------------------------
// Dispatcher
// ---------------------------------------------------------------------------

ggmcp::Context http_context(const std::string& session_id = "") {
    ggmcp::Context ctx;
    ctx.transport = "http";
    ctx.peer = "127.0.0.1:1234";
    ctx.session_id = session_id;
    return ctx;
}

ggmcp::Context stdio_context() {
    ggmcp::Context ctx;
    ctx.transport = "stdio";
    ctx.peer = "-";
    return ctx;
}

std::string initialize_body(const std::string& version = "2025-06-18") {
    return std::string("{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"initialize\",\"params\":{\"protocolVersion\":\"") +
           version + "\",\"clientInfo\":{\"name\":\"unit-test\",\"version\":\"9\"},\"capabilities\":{}}}";
}

void test_initialize() {
    group("dispatcher/initialize");

    ggmcp::Dispatcher dispatcher("test-server", "1.2.3");
    ggmcp::register_demo_tools(dispatcher);

    ggmcp::Context ctx = http_context();
    const ggmcp::Reply reply = dispatcher.handle(initialize_body(), ctx);

    check_eq(std::to_string(reply.status), "200", "initialize returns HTTP 200");
    check(!reply.session_id.empty(), "initialize issues a session id");
    check(dispatcher.session_exists(reply.session_id), "session is registered");

    ggmcp::Value message;
    std::string error;
    check(ggmcp::try_parse(reply.body, message, error), "initialize reply is valid JSON");

    const ggmcp::Value* result = find(message, "result");
    check(result != nullptr, "initialize returns a result member");
    if (!result) return;

    check_eq(find(result, "protocolVersion")->get<std::string>(), "2025-06-18", "echoes a supported version");
    check(find(result, "capabilities")->contains("tools"), "advertises the tools capability");
    check_eq(find(find(result, "serverInfo"), "name")->get<std::string>(), "test-server", "reports serverInfo.name");
    check_eq(std::to_string(find(message, "id")->get<std::int64_t>()), "1", "echoes the request id");
    check(!message.contains("error"), "initialize has no error member");

    // An unsupported version must fall back to the newest one we speak.
    ggmcp::Context other = http_context();
    const ggmcp::Reply fallback = dispatcher.handle(initialize_body("1999-01-01"), other);
    ggmcp::Value fallback_message;
    check(ggmcp::try_parse(fallback.body, fallback_message, error), "fallback reply parses");
    check_eq(find(find(fallback_message, "result"), "protocolVersion")->get<std::string>(),
             ggmcp::latest_protocol_version(), "unknown version falls back to the latest supported");
}

void test_handshake_ordering() {
    group("dispatcher/ordering (strict)");

    ggmcp::Dispatcher dispatcher("test-server", "1");
    dispatcher.session_policy(ggmcp::SessionPolicy::Strict);
    ggmcp::register_demo_tools(dispatcher);

    // Under Strict a request with no session is refused at the transport level.
    const ggmcp::Reply early = dispatcher.handle("{\"jsonrpc\":\"2.0\",\"id\":7,\"method\":\"tools/list\"}",
                                                 http_context());
    check_eq(std::to_string(early.status), "404", "tools/list without a session id is 404");
    check_has(early.body, "-32002", "the refusal carries the not-initialized code");

    ggmcp::Context ctx = http_context();
    const ggmcp::Reply init = dispatcher.handle(initialize_body(), ctx);

    // Before notifications/initialized the session is not usable...
    const ggmcp::Reply too_soon =
        dispatcher.handle("{\"jsonrpc\":\"2.0\",\"id\":8,\"method\":\"tools/list\"}",
                          http_context(init.session_id));
    check_eq(std::to_string(too_soon.status), "200", "a pre-initialized request is a JSON-RPC error");
    check_has(too_soon.body, "not initialized", "and says so");

    // ...except ping, which is allowed at any time.
    const ggmcp::Reply ping = dispatcher.handle("{\"jsonrpc\":\"2.0\",\"id\":9,\"method\":\"ping\"}",
                                                http_context(init.session_id));
    check_has(ping.body, "\"result\":{}", "ping succeeds before initialization");

    // The notification must produce no body at all (HTTP 202).
    const ggmcp::Reply note =
        dispatcher.handle("{\"jsonrpc\":\"2.0\",\"method\":\"notifications/initialized\"}",
                          http_context(init.session_id));
    check(!note.has_body, "notifications/initialized has no response body");
    check_eq(std::to_string(note.status), "202", "notifications/initialized is 202");

    const ggmcp::Reply list = dispatcher.handle("{\"jsonrpc\":\"2.0\",\"id\":10,\"method\":\"tools/list\"}",
                                                http_context(init.session_id));
    check_eq(std::to_string(list.status), "200", "tools/list works after the handshake");
    check_has(list.body, "\"tools\"", "tools/list carries a tools array");
}

void test_tools_list() {
    group("dispatcher/tools_list");

    ggmcp::Dispatcher dispatcher("test-server", "1");
    ggmcp::register_demo_tools(dispatcher);

    // The default policy is lenient, so no handshake is required here.
    const ggmcp::Reply reply = dispatcher.handle("{\"jsonrpc\":\"2.0\",\"id\":2,\"method\":\"tools/list\"}",
                                                 http_context());

    ggmcp::Value message;
    std::string error;
    check(ggmcp::try_parse(reply.body, message, error), "tools/list reply parses");

    const ggmcp::Value* tools = find(find(message, "result"), "tools");
    check(tools && tools->is_array(), "tools is an array");
    if (!tools) return;
    check_eq(std::to_string(tools->size()), "4", "four demo tools are listed");

    for (std::size_t i = 0; i < tools->size(); ++i) {
        const ggmcp::Value& tool = tools->at(i);
        check(find(tool, "name") && find(tool, "name")->is_string(), "tool has a name");
        check(find(tool, "description") && find(tool, "description")->is_string(), "tool has a description");
        const ggmcp::Value* schema = find(tool, "inputSchema");
        check(schema && schema->is_object(), "tool has an inputSchema object");
        if (schema) {
            check(find(schema, "type") && find(schema, "type")->get<std::string>() == "object",
                  "inputSchema.type is object");
            check(find(schema, "properties") && find(schema, "properties")->is_object(),
                  "inputSchema has properties");
            check(find(schema, "required") && find(schema, "required")->is_array(),
                  "inputSchema has a required array");
        }
    }
}

void test_tools_call() {
    group("dispatcher/tools_call");

    ggmcp::Dispatcher dispatcher("test-server", "1");
    ggmcp::register_demo_tools(dispatcher);

    ggmcp::Context ctx = http_context();
    const ggmcp::Reply init = dispatcher.handle(initialize_body(), ctx);
    const std::string session = init.session_id;
    ggmcp::Context session_ctx = http_context(session);
    dispatcher.handle("{\"jsonrpc\":\"2.0\",\"method\":\"notifications/initialized\"}", session_ctx);

    ggmcp::Value message;
    std::string error;

    // hello with no arguments greets the world.
    const ggmcp::Reply hello = dispatcher.handle(
        "{\"jsonrpc\":\"2.0\",\"id\":3,\"method\":\"tools/call\",\"params\":{\"name\":\"hello\","
        "\"arguments\":{}}}",
        http_context(session));
    check(ggmcp::try_parse(hello.body, message, error), "hello reply parses");
    const ggmcp::Value* content =find(find(message, "result"), "content");
    check(content && content->is_array() && content->size() == 1, "hello returns one content block");
    check_eq(find(content->at(0), "type")->get<std::string>(), "text", "content block type is text");
    check_has(find(content->at(0), "text")->get<std::string>(), "Hello, world!", "hello greets the world");
    check(!find(find(message, "result"), "isError")->get<bool>(), "hello is not an error");

    // hello with a name and loud=true.
    const ggmcp::Reply loud = dispatcher.handle(
        "{\"jsonrpc\":\"2.0\",\"id\":4,\"method\":\"tools/call\",\"params\":{\"name\":\"hello\","
        "\"arguments\":{\"name\":\"Ada Lovelace\",\"loud\":true}}}",
        http_context(session));
    check(ggmcp::try_parse(loud.body, message, error), "loud reply parses");
    const std::string loud_text = find(find(find(message, "result"), "content")->at(0), "text")->get<std::string>();
    check_has(loud_text, "HELLO, ADA LOVELACE!", "loud greets in upper case");
    check_has(loud_text, "http", "the greeting names the transport");

    // Non-ASCII travels through the whole stack.
    const ggmcp::Reply unicode = dispatcher.handle(
        "{\"jsonrpc\":\"2.0\",\"id\":5,\"method\":\"tools/call\",\"params\":{\"name\":\"echo\","
        "\"arguments\":{\"message\":\"caf\\u00e9 \\ud83d\\ude80\"}}}",
        http_context(session));
    check(ggmcp::try_parse(unicode.body, message, error), "unicode echo reply parses");
    check_eq(find(find(find(message, "result"), "content")->at(0), "text")->get<std::string>(),
             "caf\xC3\xA9 \xF0\x9F\x9A\x80", "echo round-trips non-ASCII");

    // A missing required argument is a tool-level error, not a protocol error.
    const ggmcp::Reply missing = dispatcher.handle(
        "{\"jsonrpc\":\"2.0\",\"id\":6,\"method\":\"tools/call\",\"params\":{\"name\":\"echo\","
        "\"arguments\":{}}}",
        http_context(session));
    check(ggmcp::try_parse(missing.body, message, error), "missing-argument reply parses");
    check(find(message, "result") != nullptr, "missing argument still returns a result");
    check(find(find(message, "result"), "isError")->get<bool>(), "missing argument sets isError");

    // An unknown tool is a JSON-RPC error at HTTP 200, so clients show the text.
    const ggmcp::Reply unknown = dispatcher.handle(
        "{\"jsonrpc\":\"2.0\",\"id\":7,\"method\":\"tools/call\",\"params\":{\"name\":\"nope\"}}",
        http_context(session));
    check_eq(std::to_string(unknown.status), "200", "unknown tool is not an HTTP error status");
    check_has(unknown.body, "\"code\":-32602", "unknown tool is -32602");
    check_has(unknown.body, "Unknown tool: nope", "unknown tool names the tool");
}

void test_framing_errors() {
    group("dispatcher/framing");

    ggmcp::Dispatcher dispatcher("test-server", "1");
    ggmcp::register_demo_tools(dispatcher);

    const ggmcp::Reply broken = dispatcher.handle("{not json", http_context());
    check_eq(std::to_string(broken.status), "400", "unparseable JSON is 400");
    check_has(broken.body, "-32700", "unparseable JSON is -32700");

    const ggmcp::Reply batch = dispatcher.handle("[{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"ping\"}]",
                                                 http_context());
    check_eq(std::to_string(batch.status), "400", "a batch is 400");
    check_has(batch.body, "batching", "a batch says why it failed");

    const ggmcp::Reply scalar = dispatcher.handle("42", http_context());
    check_eq(std::to_string(scalar.status), "400", "a bare scalar is 400");

    const ggmcp::Reply legacy = dispatcher.handle("{\"jsonrpc\":\"1.0\",\"id\":1,\"method\":\"ping\"}",
                                                  http_context());
    check_eq(std::to_string(legacy.status), "400", "JSON-RPC 1.0 is refused");

    const ggmcp::Reply no_method = dispatcher.handle("{\"jsonrpc\":\"2.0\",\"id\":1}", http_context());
    check_eq(std::to_string(no_method.status), "400", "a message with no method is 400");

    // Session validation runs before method dispatch, so reaching the
    // method-not-found path needs a live session.
    ggmcp::Context live = http_context();
    const ggmcp::Reply init = dispatcher.handle(initialize_body(), live);
    dispatcher.handle("{\"jsonrpc\":\"2.0\",\"method\":\"notifications/initialized\"}",
                      http_context(init.session_id));

    const ggmcp::Reply unknown_method =
        dispatcher.handle("{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"tools/teleport\"}",
                          http_context(init.session_id));
    check_eq(std::to_string(unknown_method.status), "200", "an unknown method is not an HTTP error");
    check_has(unknown_method.body, "Method not found", "an unknown method is reported");

    // (Unknown-session handling is policy-dependent; see test_session_policies.)
}

void test_stdio_skips_session_checks() {
    group("dispatcher/stdio");

    ggmcp::Dispatcher dispatcher("test-server", "1");
    ggmcp::register_demo_tools(dispatcher);

    ggmcp::Context ctx = stdio_context();
    const ggmcp::Reply init = dispatcher.handle(initialize_body(), ctx);
    check_eq(std::to_string(init.status), "200", "stdio initialize is 200");

    // stdio is one implicit session per process, so there is nothing to send back.
    const ggmcp::Reply list = dispatcher.handle("{\"jsonrpc\":\"2.0\",\"id\":2,\"method\":\"tools/list\"}",
                                                stdio_context());
    check_has(list.body, "\"tools\"", "stdio needs no session id");
}

// ---------------------------------------------------------------------------
// Origin validation
// ---------------------------------------------------------------------------

void test_origins() {
    group("http/origin");

    check(ggmcp::origin_is_loopback("http://localhost:8080", 8080), "localhost:8080 allowed on 8080");
    check(ggmcp::origin_is_loopback("http://127.0.0.1:8080", 8080), "127.0.0.1 allowed");
    check(ggmcp::origin_is_loopback("http://[::1]:8080", 8080), "IPv6 loopback allowed");
    check(ggmcp::origin_is_loopback("http://localhost", 8080), "a portless loopback origin allowed");
    check(!ggmcp::origin_is_loopback("http://localhost:9999", 8080), "a wrong port is rejected");
    check(!ggmcp::origin_is_loopback("http://evil.example", 8080), "a remote origin is rejected");
    check(!ggmcp::origin_is_loopback("http://127.0.0.1.evil.example", 8080), "a spoofed prefix is rejected");
    check(!ggmcp::origin_is_loopback("not-a-origin", 8080), "a malformed origin is rejected");
    check(!ggmcp::origin_is_loopback("ftp://localhost:8080", 8080), "an unsupported scheme is rejected");
}

}  // namespace

/// The Mcp-Session-Id header is optional for the server per spec, so the three
/// policies have distinct and separately-tested behaviour.
void test_session_policies() {
    group("dispatcher/session-policy");

    // --- the default: lenient -------------------------------------------
    ggmcp::Dispatcher lenient("test-server", "1");
    ggmcp::register_demo_tools(lenient);
    check(std::string(ggmcp::to_string(lenient.session_policy())) == "lenient",
          "the default session policy is lenient");

    const ggmcp::Reply no_session =
        lenient.handle("{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"tools/list\"}", http_context());
    check_eq(std::to_string(no_session.status), "200", "lenient serves a sessionless tools/list");
    check_has(no_session.body, "\"tools\"", "and returns the tool list");

    const ggmcp::Reply bogus =
        lenient.handle("{\"jsonrpc\":\"2.0\",\"id\":2,\"method\":\"tools/list\"}", http_context("no-such-id"));
    check_eq(std::to_string(bogus.status), "200", "lenient serves an unrecognised session id");

    // A real handshake still yields a real session that reports clientInfo.
    const ggmcp::Reply init = lenient.handle(initialize_body(), http_context());
    check(!init.session_id.empty(), "lenient still issues a session id");
    const ggmcp::Reply known =
        lenient.handle("{\"jsonrpc\":\"2.0\",\"id\":3,\"method\":\"tools/list\"}", http_context(init.session_id));
    check_eq(std::to_string(known.status), "200", "a known session works");

    // --- strict ---------------------------------------------------------
    ggmcp::Dispatcher strict("test-server", "1");
    strict.session_policy(ggmcp::SessionPolicy::Strict);
    ggmcp::register_demo_tools(strict);

    const ggmcp::Reply rejected =
        strict.handle("{\"jsonrpc\":\"2.0\",\"id\":4,\"method\":\"tools/list\"}", http_context());
    check_eq(std::to_string(rejected.status), "404", "strict rejects a sessionless request");
    check_has(rejected.body, "-32002", "strict rejection carries the not-initialized code");

    const ggmcp::Reply strict_unknown =
        strict.handle("{\"jsonrpc\":\"2.0\",\"id\":5,\"method\":\"tools/list\"}", http_context("no-such-id"));
    check_eq(std::to_string(strict_unknown.status), "404", "strict rejects an unknown session id");

    const ggmcp::Reply strict_init = strict.handle(initialize_body(), http_context());
    const ggmcp::Reply too_soon =
        strict.handle("{\"jsonrpc\":\"2.0\",\"id\":6,\"method\":\"tools/list\"}", http_context(strict_init.session_id));
    check_has(too_soon.body, "not initialized", "strict demands the handshake notification");

    strict.handle("{\"jsonrpc\":\"2.0\",\"method\":\"notifications/initialized\"}",
                  http_context(strict_init.session_id));
    const ggmcp::Reply after =
        strict.handle("{\"jsonrpc\":\"2.0\",\"id\":7,\"method\":\"tools/list\"}", http_context(strict_init.session_id));
    check_eq(std::to_string(after.status), "200", "strict serves requests after the handshake");

    // --- stateless ------------------------------------------------------
    ggmcp::Dispatcher stateless("test-server", "1");
    stateless.session_policy(ggmcp::SessionPolicy::Stateless);
    ggmcp::register_demo_tools(stateless);

    const ggmcp::Reply stateless_init = stateless.handle(initialize_body(), http_context());
    check_eq(std::to_string(stateless_init.status), "200", "stateless initialize succeeds");
    check(stateless_init.session_id.empty(), "stateless issues no session id");
    check_eq(std::to_string(stateless.session_count()), "0", "stateless stores no sessions");
    check_has(stateless_init.body, "protocolVersion", "stateless still negotiates a version");

    const ggmcp::Reply stateless_list =
        stateless.handle("{\"jsonrpc\":\"2.0\",\"id\":8,\"method\":\"tools/list\"}", http_context());
    check_eq(std::to_string(stateless_list.status), "200", "stateless serves tools/list");

    // --- name parsing ---------------------------------------------------
    ggmcp::SessionPolicy parsed = ggmcp::SessionPolicy::Lenient;
    check(ggmcp::parse_session_policy("strict", parsed) &&
              std::string(ggmcp::to_string(parsed)) == "strict",
          "parse_session_policy accepts strict");
    check(ggmcp::parse_session_policy("STATELESS", parsed) &&
              std::string(ggmcp::to_string(parsed)) == "stateless",
          "parse_session_policy is case-insensitive");
    parsed = ggmcp::SessionPolicy::Lenient;
    check(!ggmcp::parse_session_policy("sideways", parsed), "parse_session_policy rejects nonsense");
    check(std::string(ggmcp::to_string(parsed)) == "lenient", "a rejected parse leaves the value alone");
}

// ---------------------------------------------------------------------------
// File and shell tools
// ---------------------------------------------------------------------------

/// A scratch tree under the temp directory, removed when the test ends.
struct ScratchSpace {
    std::filesystem::path root;   ///< canonicalised: macOS /var is a link to /private/var
    std::filesystem::path outside;

    explicit ScratchSpace(const std::string& name) {
        std::error_code ec;
        root = std::filesystem::temp_directory_path() / (name + "-root");
        outside = std::filesystem::temp_directory_path() / (name + "-outside");
        std::filesystem::remove_all(root, ec);
        std::filesystem::remove_all(outside, ec);
        std::filesystem::create_directories(root, ec);
        std::filesystem::create_directories(outside, ec);
        {
            std::ofstream secret(outside / "secret.txt", std::ios::binary);
            secret << "do not read this";
        }
        root = std::filesystem::canonical(root, ec);
        outside = std::filesystem::canonical(outside, ec);
        std::filesystem::create_symlink(outside / "secret.txt", root / "link.txt", ec);
    }

    ~ScratchSpace() {
        std::error_code ec;
        std::filesystem::remove_all(root, ec);
        std::filesystem::remove_all(outside, ec);
    }

    std::string text(const std::string& relative) const {
        std::error_code ec;
        std::ifstream stream(root / relative, std::ios::binary);
        std::string content;
        if (stream) {
            std::ostringstream buffer;
            buffer << stream.rdbuf();
            content = buffer.str();
        }
        return content;
    }
};

/// Outcome of a tools/call, flattened out of the JSON-RPC envelope.
struct ToolCall {
    bool is_error = false;
    std::string text;
    int status = 0;
    std::string raw;
};

ToolCall call_tool(ggmcp::Dispatcher& dispatcher, const std::string& session, const std::string& name,
                   const std::string& arguments) {
    ToolCall call;
    const ggmcp::Reply reply = dispatcher.handle(
        "{\"jsonrpc\":\"2.0\",\"id\":99,\"method\":\"tools/call\",\"params\":{\"name\":\"" + name +
            "\",\"arguments\":" + arguments + "}}",
        http_context(session));
    call.status = reply.status;
    call.raw = reply.body;

    ggmcp::Value message;
    std::string error;
    if (!ggmcp::try_parse(reply.body, message, error)) {
        call.is_error = true;
        return call;
    }

    const ggmcp::Value* jsonrpc_error = find(message, "error");
    if (jsonrpc_error != nullptr) {   // an unknown tool is a protocol error, not a tool error
        call.is_error = true;
        const ggmcp::Value* message_text = find(jsonrpc_error, "message");
        if (message_text != nullptr && message_text->is_string()) call.text = message_text->get<std::string>();
        return call;
    }

    const ggmcp::Value* result = find(message, "result");
    const ggmcp::Value* is_error = result == nullptr ? nullptr : find(result, "isError");
    call.is_error = is_error != nullptr && is_error->get<bool>();
    const ggmcp::Value* content = result == nullptr ? nullptr : find(result, "content");
    if (content != nullptr && content->is_array() && content->size() > 0) {
        const ggmcp::Value* text = find(content->at(0), "text");
        if (text != nullptr && text->is_string()) call.text = text->get<std::string>();
    }
    return call;
}

// ---------------------------------------------------------------------------
// Authorization
// ---------------------------------------------------------------------------

/// Builds a Request the way the HTTP parser would, so the auth checks can be
/// driven without a socket.
ggmcp::Request auth_request(const std::string& target, const std::string& authorization,
                            const std::string& method = "POST") {
    ggmcp::Request request;
    request.method = method;
    request.target = target;
    const std::size_t question = target.find('?');
    request.path = question == std::string::npos ? target : target.substr(0, question);
    request.query = question == std::string::npos ? std::string() : target.substr(question + 1);
    request.version = "HTTP/1.1";
    request.peer = "127.0.0.1:43111";
    request.body = "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"tools/list\",\"params\":{}}";
    request.headers["content-type"] = "application/json";
    request.headers["accept"] = "application/json";
    if (!authorization.empty()) request.headers["authorization"] = authorization;
    return request;
}

std::string response_header(const ggmcp::Response& response, const std::string& name) {
    for (const auto& header : response.headers) {
        std::string key = header.first;
        for (char& c : key) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        if (key == name) return header.second;
    }
    return {};
}

void write_tokens(const std::filesystem::path& path, const std::string& contents) {
    std::ofstream stream(path, std::ios::binary);
    stream << contents;
}

void test_constant_time_equals() {
    group("auth/constant-time");

    // These pin the *behaviour*; they cannot pin the timing, which is the whole
    // point. A short-circuiting `a == b` passes every assertion here and still
    // hands a caller the secret one byte at a time - which is why the loop below
    // exists at all, and why it must not be "simplified" back into an ==.
    check(ggmcp::constant_time_equals("", ""), "two empty strings match");
    check(ggmcp::constant_time_equals("token-value", "token-value"), "equal secrets match");
    check(!ggmcp::constant_time_equals("token-value", "token-valuE"), "one byte at the end differs");
    check(!ggmcp::constant_time_equals("token-value", "Token-value"), "one byte at the start differs");
    check(!ggmcp::constant_time_equals("token", "token-value"), "a prefix does not match");
    check(!ggmcp::constant_time_equals("token-value", "token"), "a longer secret does not match");
    check(!ggmcp::constant_time_equals("token-value", ""), "the empty string matches nothing real");

    std::string long_secret(4096, 'x');
    std::string differs_at_the_end = long_secret;
    differs_at_the_end.back() = 'y';
    check(!ggmcp::constant_time_equals(long_secret, differs_at_the_end), "a long secret differs in its last byte");
    differs_at_the_end.back() = 'x';
    check(ggmcp::constant_time_equals(long_secret, differs_at_the_end), "and matches once the byte is put back");
}

void test_token_store() {
    group("auth/tokens");

    ScratchSpace scratch("ggmcp-auth");
    const std::string secret = "unit-test-token-aaaaaaaa";
    const std::string other = "unit-test-token-bbbbbbbb";
    const std::filesystem::path tokens = scratch.root / "tokens.txt";
    std::string error;

    // --- loading ---------------------------------------------------------
    ggmcp::AuthOptions literal;
    literal.required = true;
    literal.token_literal = secret;
    ggmcp::TokenStore store;
    check(ggmcp::TokenStore::prepare(literal, store, error), "a handed-over token is accepted");
    check(store.required(), "the store knows it is enforcing");
    check_eq(std::to_string(store.size()), "1", "one token loaded");
    check_eq(store.labels().empty() ? "?" : store.labels()[0], "stdin", "the source is labelled, not quoted");

    ggmcp::AuthOptions off;
    ggmcp::TokenStore open_store;
    check(ggmcp::TokenStore::prepare(off, open_store, error), "an unrequired store prepares");
    check(!open_store.required(), "and reports itself unrequired");
    check_eq(std::to_string(open_store.size()), "0", "with nothing to check against");
    check(open_store.authorize(auth_request("/mcp", "")).ok, "and lets anonymous traffic through");

    // An empty file is not "no tokens configured", it is a mistake, and the
    // operator should hear about it at startup rather than at the door.
    write_tokens(tokens, "# nothing but a comment\n\n");
    ggmcp::AuthOptions empty_file;
    empty_file.required = true;
    empty_file.token_files = {tokens.string()};
    ggmcp::TokenStore empty_store;
    check(!ggmcp::TokenStore::prepare(empty_file, empty_store, error), "a file with no tokens fails to load");
    check_has(error, "contains no tokens", "and says so");

    write_tokens(tokens, "missing-file-is-not-created\n");
    std::filesystem::remove(tokens);
    ggmcp::AuthOptions missing;
    missing.required = true;
    missing.token_files = {tokens.string()};
    ggmcp::TokenStore missing_store;
    check(!ggmcp::TokenStore::prepare(missing, missing_store, error), "a missing file fails to load");
    check_has(error, "could not be opened", "and says so");

    // $GGMCP_TOKEN is the default source; naming any other source must not also
    // silently demand it.
    // A comment line, a blank line, then two tokens: lines 3 and 4.
    write_tokens(tokens, "# one token per line\n\n" + secret + "\n" + other + "\n");
    ggmcp::TokenStore file_store;
    ggmcp::AuthOptions file_only;
    file_only.required = true;
    file_only.token_files = {tokens.string()};
    check(ggmcp::TokenStore::prepare(file_only, file_store, error),
          "a file-only configuration does not also require $GGMCP_TOKEN");
    check_eq(std::to_string(file_store.size()), "2", "one token per line, comments and blanks skipped");

    // --- the check -------------------------------------------------------
    const ggmcp::AuthResult anonymous = file_store.authorize(auth_request("/mcp", ""));
    check(!anonymous.ok, "no credential is turned away");
    check_eq(anonymous.oauth_error, "invalid_request", "a missing credential is invalid_request");
    check(!contains(anonymous.description, secret), "the refusal names no real token");

    const ggmcp::AuthResult foreign_scheme = file_store.authorize(auth_request("/mcp", "Basic Zm9vOmJhcg=="));
    check(!foreign_scheme.ok, "a foreign scheme is turned away");
    check_has(foreign_scheme.description, "Bearer", "and the refusal names the scheme it wants");

    const ggmcp::AuthResult wrong = file_store.authorize(auth_request("/mcp", "Bearer nope-not-it"));
    check(!wrong.ok, "a wrong token is turned away");
    check_eq(wrong.oauth_error, "invalid_token", "a wrong token is invalid_token");
    check(!contains(wrong.description, "nope-not-it"), "and the token is not echoed back");

    const ggmcp::AuthResult right = file_store.authorize(auth_request("/mcp", "Bearer " + secret));
    check(right.ok, "the right token is let in");
    check(!right.identity.anonymous, "and the caller stops being anonymous");
    check_has(right.identity.describe(), "tokens.txt:3", "with a label naming the source line");
    check(!contains(right.identity.describe(), secret), "the label is a source, not the secret");

    check(file_store.authorize(auth_request("/mcp", "bearer " + secret)).ok,
          "the scheme name is matched case-insensitively");
    check(file_store.authorize(auth_request("/mcp", "Bearer   " + secret + "  ")).ok,
          "surrounding spaces are tolerated");
    check(!file_store.authorize(auth_request("/mcp", "Bearer ")).ok, "a scheme with no token is refused");

    // --- no side door through the URL ------------------------------------
    for (const char* name : {"access_token", "token", "bearer", "id_token", "api_key", "apiKey", "key",
                             "secret", "client_secret", "authorization"}) {
        const std::string target = std::string("/mcp?") + name + "=" + secret;
        const ggmcp::AuthResult smuggled = file_store.authorize(auth_request(target, "Bearer " + secret));
        check(!smuggled.ok, std::string("a valid bearer token does not license ?") + name + "= in the URL");
        check_has(smuggled.description, "query string", "and the refusal explains why");
    }
    check(file_store.authorize(auth_request("/mcp?cursor=abc&limit=10", "Bearer " + secret)).ok,
          "an ordinary query parameter is not a credential");

    // --- rotation --------------------------------------------------------
    write_tokens(tokens, other + "\n");
    check_eq(std::to_string(file_store.size()), "1", "the store notices the file changed");
    check(!file_store.authorize(auth_request("/mcp", "Bearer " + secret)).ok, "the revoked token stops working");
    check(file_store.authorize(auth_request("/mcp", "Bearer " + other)).ok, "the new token works with no restart");

    std::filesystem::remove(tokens);
    check(file_store.authorize(auth_request("/mcp", "Bearer " + other)).ok,
          "a vanished token file keeps the tokens it last held rather than locking everyone out");

    // --- the documents an unauthenticated client sees --------------------
    const std::string metadata = file_store.metadata_json("http://127.0.0.1:8080/mcp");
    check_has(metadata, "\"resource\":\"http://127.0.0.1:8080/mcp\"", "the metadata names the resource");
    check_has(metadata, "bearer_methods_supported", "and the bearer methods");
    check(!contains(metadata, "authorization_servers"), "and advertises no authorization server by default");
    check(!contains(metadata, secret) && !contains(metadata, other), "the metadata carries no secrets");

    const std::string challenge = file_store.www_authenticate("http://h/.well-known/x", "invalid_token", "nope");
    check_has(challenge, "Bearer realm=\"ggmcp\"", "the challenge names the scheme and realm");
    check_has(challenge, "error=\"invalid_token\"", "the challenge carries the error code");
    check_has(challenge, "resource_metadata=\"http://h/.well-known/x\"", "and the metadata URL");

    ggmcp::AuthOptions hostile;
    hostile.required = true;
    hostile.token_literal = secret;
    hostile.realm = "evil\", error=\"injected";
    hostile.public_url = "https://proxy.example/base/";
    ggmcp::TokenStore hostile_store;
    check(ggmcp::TokenStore::prepare(hostile, hostile_store, error), "a hostile realm still loads");
    check_eq(hostile_store.public_url(), "https://proxy.example/base", "the public URL loses its trailing slash");
    const std::string escaped = hostile_store.www_authenticate("/.well-known/x", "invalid_request", "d");
    check(!contains(escaped, "error=\"injected\""), "a quote in the realm cannot inject a parameter");
    check_has(escaped, "\\\"", "and is escaped instead");

    ggmcp::AuthOptions with_as;
    with_as.required = true;
    with_as.token_literal = secret;
    with_as.authorization_servers = {"https://auth.example"};
    ggmcp::TokenStore as_store;
    check(ggmcp::TokenStore::prepare(with_as, as_store, error), "an authorization server can be named");
    check_has(as_store.metadata_json("http://h/mcp"), "https://auth.example", "and is advertised verbatim");
    check(as_store.authorize(auth_request("/mcp", "Bearer " + secret)).ok,
          "naming one does not change what this server accepts");
}

void test_authorization_handler() {
    group("auth/handler");

    const std::string secret = "handler-test-token-aaa";
    std::string error;
    ggmcp::AuthOptions auth;
    auth.required = true;
    auth.token_literal = secret;

    auto store = std::make_shared<ggmcp::TokenStore>();
    check(ggmcp::TokenStore::prepare(auth, *store, error), "the handler's store prepares");

    ggmcp::Dispatcher dispatcher("test-server", "1");
    ggmcp::register_demo_tools(dispatcher);

    ggmcp::HttpOptions http;
    http.tokens = store;
    const ggmcp::HttpServer::Handler handler = ggmcp::make_http_handler(dispatcher, http, 8080);

    const ggmcp::Response anonymous = handler(auth_request("/mcp", ""));
    check_eq(std::to_string(anonymous.status), "401", "the handler refuses an anonymous call");
    check_has(response_header(anonymous, "www-authenticate"), "resource_metadata=", "and points at the metadata");
    check_has(response_header(anonymous, "www-authenticate"), "invalid_request", "with the RFC 6750 error code");
    check_eq(response_header(anonymous, "cache-control"), "no-store", "a 401 is not cacheable");
    check_eq(response_header(anonymous, "x-content-type-options"), "nosniff", "and is not sniffable");
    check(!contains(anonymous.body, secret), "the 401 body carries no secrets");

    const ggmcp::Response served = handler(auth_request("/mcp", "Bearer " + secret));
    check_eq(std::to_string(served.status), "200", "a valid token is served");
    check_has(served.body, "\"tools\"", "with the tool list");

    // The only two exemptions, and why each is one.
    const ggmcp::Response preflight = handler(auth_request("/mcp", "", "OPTIONS"));
    check_eq(std::to_string(preflight.status), "204", "the CORS preflight needs no credential");
    const ggmcp::Response metadata =
        handler(auth_request("/.well-known/oauth-protected-resource", "", "GET"));
    check_eq(std::to_string(metadata.status), "200", "the metadata document needs none either");
    check_has(metadata.body, "\"resource\":\"http://127.0.0.1:8080/mcp\"", "and names the bound resource");

    // Which tools exist is configuration, so both convenience routes are gated.
    check_eq(std::to_string(handler(auth_request("/health", "", "GET")).status), "401", "/health is gated");
    check_eq(std::to_string(handler(auth_request("/", "", "GET")).status), "401", "the index is gated");
    const ggmcp::Response health = handler(auth_request("/health", "Bearer " + secret, "GET"));
    check_has(health.body, "\"authorization\":\"bearer\"", "health reports the mode");
    check_has(handler(auth_request("/", "Bearer " + secret, "GET")).body, "Authorization is required",
              "the index tells callers what to send");

    // What gets advertised as the resource: the Host header when a proxy put one
    // there, the bound address otherwise, and a Host header that is not a bare
    // authority never at all - it lands in a header a client may follow.
    ggmcp::Request proxied = auth_request("/mcp", "Bearer " + secret, "GET");
    proxied.headers["host"] = "mcp.example.test:8443";
    check_eq(std::to_string(handler(proxied).status), "405", "GET on the endpoint is 405 with a valid token");

    ggmcp::Request with_host = auth_request("/mcp", "");
    with_host.headers["host"] = "mcp.example.test:8443";
    check_has(response_header(handler(with_host), "www-authenticate"), "http://mcp.example.test:8443/",
              "a Host header becomes the advertised resource");

    ggmcp::Request evil_host = auth_request("/mcp", "");
    evil_host.headers["host"] = "evil.example/redirect";
    check(!contains(response_header(handler(evil_host), "www-authenticate"), "evil.example"),
          "a Host header that is not a bare authority is not echoed");
    check_has(response_header(handler(evil_host), "www-authenticate"), "http://127.0.0.1:8080/",
              "and it falls back to the bound address");

    // With no store configured the wire is exactly what it was before this option.
    ggmcp::HttpOptions open;
    const ggmcp::HttpServer::Handler open_handler = ggmcp::make_http_handler(dispatcher, open, 8080);
    check_eq(std::to_string(open_handler(auth_request("/mcp", "")).status), "200", "no store means no gate");
    check_eq(std::to_string(open_handler(auth_request("/health", "", "GET")).status), "200",
             "and health stays public");
    check_eq(std::to_string(open_handler(auth_request("/.well-known/oauth-protected-resource", "", "GET")).status),
             "404", "and no metadata document is served by an open server");
    check_has(open_handler(auth_request("/health", "", "GET")).body, "\"anonymous\"", "health says anonymous");
}

void test_path_confinement() {
    group("tools/confinement");

    ScratchSpace scratch("ggmcp-confine");
    const std::string root = scratch.root.string();
    std::string resolved;
    std::string error;

    check(ggmcp::confine_path(root, "a.txt", false, resolved, error), "a relative path is accepted");
    check_eq(resolved, root + "/a.txt", "a relative path resolves under the root");

    check(ggmcp::confine_path(root, root + "/a.txt", false, resolved, error), "an in-root absolute path is accepted");
    check_eq(resolved, root + "/a.txt", "an absolute in-root path is unchanged");

    error.clear();
    check(!ggmcp::confine_path(root, "../a.txt", false, resolved, error), "'..' out of the root is rejected");
    check(!error.empty(), "a rejected path explains itself");

    check(!ggmcp::confine_path(root, "/etc/passwd", false, resolved, error), "an out-of-root absolute path is rejected");
    check(!ggmcp::confine_path(root, "", false, resolved, error), "an empty path is rejected");

    check(ggmcp::confine_path(root, "sub/./a.txt", false, resolved, error), "'.' components are accepted");
    check(ggmcp::confine_path(root, "deep/deeper/a.txt", false, resolved, error),
          "missing intermediate directories are tolerated");
    check_eq(resolved, root + "/deep/deeper/a.txt", "a missing tree resolves verbatim");

    // The point of canonicalising before checking: a link that leaves the tree
    // must fail even though the path the caller typed looks local.
    check(!ggmcp::confine_path(root, "link.txt", false, resolved, error),
          "a symlink pointing out of the root is rejected");

    check(ggmcp::confine_path(root, "../../etc/passwd", true, resolved, error),
          "allow_outside_root lets an escaping path through");
}

void test_filesystem_tools() {
    group("tools/file");

    ScratchSpace scratch("ggmcp-file");
    ggmcp::FileSystemOptions options;
    options.root = scratch.root.string();
    options.enable_file_tools = true;

    ggmcp::Dispatcher dispatcher("test-server", "1");
    ggmcp::register_demo_tools(dispatcher);
    ggmcp::register_filesystem_tools(dispatcher, options);
    check_eq(std::to_string(dispatcher.tool_names().size()), "7",
             "read/write/edit join the four demo tools");
    check(!dispatcher.has_tool("bash"), "bash stays off without --allow-bash");

    // tools/list advertises the new tools with schemas.
    const ggmcp::Reply listed =
        dispatcher.handle("{\"jsonrpc\":\"2.0\",\"id\":2,\"method\":\"tools/list\",\"params\":{}}",
                          http_context("s"));
    check_has(listed.body, "\"name\":\"read\"", "tools/list includes read");
    check_has(listed.body, "\"name\":\"write\"", "tools/list includes write");
    check_has(listed.body, "\"name\":\"edit\"", "tools/list includes edit");
    check_has(listed.body, "readOnlyHint", "tools/list carries annotations");

    // write creates a file, and creates parents on the way.
    const ToolCall write = call_tool(dispatcher, "s", "write", "{\"path\":\"a.txt\",\"content\":\"one\\n\"}");
    check(!write.is_error, "write succeeds");
    check_has(write.text, "wrote 4 bytes", "write reports the byte count");
    check_eq(scratch.text("a.txt"), "one\n", "write put the bytes on disk");

    const ToolCall nested =
        call_tool(dispatcher, "s", "write", "{\"path\":\"x/y/z.txt\",\"content\":\"nested\"}");
    check(!nested.is_error, "write creates missing parent directories");
    check_eq(scratch.text("x/y/z.txt"), "nested", "the nested file landed");

    const ToolCall append =
        call_tool(dispatcher, "s", "write", "{\"path\":\"a.txt\",\"content\":\"two\\n\",\"append\":true}");
    check(!append.is_error, "append succeeds");
    check_eq(scratch.text("a.txt"), "one\ntwo\n", "append added to the end");

    // read returns it, and slices by line.
    const ToolCall read = call_tool(dispatcher, "s", "read", "{\"path\":\"a.txt\"}");
    check(!read.is_error, "read succeeds");
    check_eq(read.text, "one\ntwo\n", "read returns the whole file");

    const ToolCall slice = call_tool(dispatcher, "s", "read", "{\"path\":\"a.txt\",\"offset\":2}");
    check(!slice.is_error, "read with an offset succeeds");
    check_eq(slice.text, "two\n", "read honours offset");

    const ToolCall too_far = call_tool(dispatcher, "s", "read", "{\"path\":\"a.txt\",\"offset\":99}");
    check(too_far.is_error, "an offset past the end is an error");
    check_has(too_far.text, "past the end", "the offset error says so");

    const ToolCall missing = call_tool(dispatcher, "s", "read", "{\"path\":\"nope.txt\"}");
    check(missing.is_error, "reading a missing file is an error");

    const ToolCall directory = call_tool(dispatcher, "s", "read", "{\"path\":\"x\"}");
    check(directory.is_error, "reading a directory is an error");

    // edit: several disjoint edits in one call.
    call_tool(dispatcher, "s", "write",
              "{\"path\":\"b.txt\",\"content\":\"alpha\\nbeta\\ngamma\\n\"}");
    const ToolCall edited = call_tool(dispatcher, "s", "edit",
                                     "{\"path\":\"b.txt\",\"edits\":[{\"oldText\":\"alpha\","
                                     "\"newText\":\"ALPHA\"},{\"oldText\":\"gamma\",\"newText\":\"OMEGA\"}]}");
    check(!edited.is_error, "a multi-edit call succeeds");
    check_eq(scratch.text("b.txt"), "ALPHA\nbeta\nOMEGA\n", "both edits applied");

    // An ambiguous or missing match leaves the file untouched.
    const ToolCall ambiguous =
        call_tool(dispatcher, "s", "edit", "{\"path\":\"b.txt\",\"edits\":[{\"oldText\":\"A\",\"newText\":\"X\"}]}");
    check(ambiguous.is_error, "an ambiguous oldText is an error");
    check_has(ambiguous.text, "more than once", "the ambiguity is explained");
    check_eq(scratch.text("b.txt"), "ALPHA\nbeta\nOMEGA\n", "an ambiguous edit changed nothing");

    const ToolCall partial =
        call_tool(dispatcher, "s", "edit", "{\"path\":\"b.txt\",\"edits\":[{\"oldText\":\"ALPHA\","
                                       "\"newText\":\"1\"},{\"oldText\":\"absent\",\"newText\":\"2\"}]}");
    check(partial.is_error, "one bad edit fails the whole call");
    check_eq(scratch.text("b.txt"), "ALPHA\nbeta\nOMEGA\n", "a failed multi-edit call is all-or-nothing");

    const ToolCall empty_old =
        call_tool(dispatcher, "s", "edit", "{\"path\":\"b.txt\",\"edits\":[{\"oldText\":\"\",\"newText\":\"X\"}]}");
    check(empty_old.is_error, "an empty oldText is rejected");

    const ToolCall no_edits = call_tool(dispatcher, "s", "edit", "{\"path\":\"b.txt\",\"edits\":[]}");
    check(no_edits.is_error, "an empty edits array is rejected");

    const ToolCall bad_type = call_tool(dispatcher, "s", "edit", "{\"path\":\"b.txt\",\"edits\":\"nope\"}");
    check(bad_type.is_error, "a non-array edits value is rejected");

    const ToolCall missing_file =
        call_tool(dispatcher, "s", "edit", "{\"path\":\"nope.txt\",\"edits\":[{\"oldText\":\"a\",\"newText\":\"b\"}]}");
    check(missing_file.is_error, "editing a missing file is an error");

    // Binary content is refused rather than encoded, and an oversized read is cut
    // with a marker so the model knows it is looking at a fragment.
    check(!call_tool(dispatcher, "s", "write", "{\"path\":\"bin.dat\",\"content\":\"\\u0000secret-bytes\"}").is_error,
          "write does not care what the bytes are");
    const ToolCall binary_read = call_tool(dispatcher, "s", "read", "{\"path\":\"bin.dat\"}");
    check(binary_read.is_error, "read refuses a file containing a NUL byte");
    check(!contains(binary_read.text, "secret-bytes"), "the refusal does not echo the payload");

    ggmcp::FileSystemOptions tiny = options;
    tiny.max_read_bytes = 8;
    ggmcp::Dispatcher tiny_dispatcher("test-server", "1");
    ggmcp::register_filesystem_tools(tiny_dispatcher, tiny);
    const ToolCall cut = call_tool(tiny_dispatcher, "s", "read", "{\"path\":\"b.txt\"}");
    check(!cut.is_error, "an oversized file is still readable");
    check_has(cut.text, "truncated at 8 of 17 bytes", "the truncation is marked and measured");
    check_eq(cut.text.substr(0, 8), "ALPHA\nbe", "and it returns exactly the cap");

    // Confinement, through the tools rather than confine_path directly.
    const ToolCall escape = call_tool(dispatcher, "s", "read", "{\"path\":\"../../etc/passwd\"}");
    check(escape.is_error, "read refuses to leave the root");
    check_has(escape.text, "outside the workspace root", "the escape error names the root");

    const ToolCall via_link = call_tool(dispatcher, "s", "read", "{\"path\":\"link.txt\"}");
    check(via_link.is_error, "read refuses to follow a symlink out of the root");
    check(!contains(via_link.text, "do not read this"), "the refusal does not leak the file");

    const ToolCall write_outside =
        call_tool(dispatcher, "s", "write", "{\"path\":\"../escape.txt\",\"content\":\"x\"}");
    check(write_outside.is_error, "write refuses to leave the root");
    check(!std::filesystem::exists(scratch.outside / "escape.txt"), "nothing was written outside");

    // Argument validation happens before any I/O.
    check(call_tool(dispatcher, "s", "write", "{\"path\":\"c.txt\"}").is_error, "write needs content");
    check(call_tool(dispatcher, "s", "read", "{\"offset\":1}").is_error, "read needs a path");
    check(call_tool(dispatcher, "s", "read", "{\"path\":42}").is_error, "read rejects a non-string path");
    check(call_tool(dispatcher, "s", "read", "{\"path\":\"a.txt\",\"offset\":0}").is_error,
          "read rejects an offset below 1");
}

void test_bash_tool() {
    group("tools/bash");

    ScratchSpace scratch("ggmcp-bash");
    ggmcp::FileSystemOptions options;
    options.root = scratch.root.string();
    options.enable_file_tools = true;
    options.enable_bash = true;
    options.bash_timeout_seconds = 2;

    ggmcp::Dispatcher dispatcher("test-server", "1");
    ggmcp::register_demo_tools(dispatcher);
    ggmcp::register_filesystem_tools(dispatcher, options);
    check_eq(std::to_string(dispatcher.tool_names().size()), "8", "bash joins once enabled");

    const ToolCall echo = call_tool(dispatcher, "s", "bash", "{\"command\":\"echo hi\"}");
    check(!echo.is_error, "a successful command is not an error");
    check_eq(echo.text, "hi", "stdout is captured");

    // stderr is merged into the same text block, and a non-zero exit is an error.
    const ToolCall failing =
        call_tool(dispatcher, "s", "bash", "{\"command\":\"echo out; echo err >&2; exit 3\"}");
    check(failing.is_error, "a non-zero exit sets isError");
    check_has(failing.text, "out", "stdout survives a failing command");
    check_has(failing.text, "err", "stderr survives a failing command");
    check_has(failing.text, "exit 3", "the exit code is reported");

    const ToolCall timeout = call_tool(dispatcher, "s", "bash", "{\"command\":\"sleep 20\",\"timeout\":1}");
    check(timeout.is_error, "a timeout is an error");
    check_has(timeout.text, "timed out", "the timeout is explained");

    const ToolCall bad_timeout = call_tool(dispatcher, "s", "bash", "{\"command\":\"echo hi\",\"timeout\":0}");
    check(bad_timeout.is_error, "a zero timeout is rejected");

    check(call_tool(dispatcher, "s", "bash", "{\"command\":\"\"}").is_error, "an empty command is rejected");
    check(call_tool(dispatcher, "s", "bash", "{}").is_error, "bash needs a command");

    // A backgrounded grandchild must not turn a finished command into a timeout.
    const ToolCall straggler =
        call_tool(dispatcher, "s", "bash", "{\"command\":\"(sleep 20 &) ; echo spawned\",\"timeout\":5}");
    check(!straggler.is_error, "a command that backgrounds a child still succeeds");
    check_has(straggler.text, "spawned", "the straggler's parent output is kept");
}

int main() {
    test_json_scalars();
    test_json_strings();
    test_json_containers();
    test_json_errors();
    test_json_depth_limit();

    test_initialize();
    test_handshake_ordering();
    test_tools_list();
    test_tools_call();
    test_framing_errors();
    test_session_policies();
    test_stdio_skips_session_checks();
    test_origins();

    test_constant_time_equals();
    test_token_store();
    test_authorization_handler();

    test_path_confinement();
    test_filesystem_tools();
    test_bash_tool();

    if (g_failures != 0) {
        std::fprintf(stderr, "\n%d of %d checks FAILED\n", g_failures, g_checks);
        return 1;
    }
    std::printf("all %d checks passed\n", g_checks);
    return 0;
}
