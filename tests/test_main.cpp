// ggmcp - a tiny Model Context Protocol server in dependency-free C++17.
//
// SPDX-License-Identifier: MIT
//
// test_main.cpp - in-process tests for the JSON codec and the MCP dispatcher.
// No sockets here; tests/http_roundtrip.py drives the HTTP transport.

#include <cstdio>
#include <functional>
#include <string>
#include <vector>

#include "ggmcp/json.hpp"
#include "ggmcp/mcp_http.hpp"
#include "ggmcp/protocol.hpp"
#include "ggmcp/tools.hpp"

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

    check(ggmcp::Value::parse("null").is_null(), "null parses");
    check(ggmcp::Value::parse("true").as_bool(), "true parses");
    check(!ggmcp::Value::parse("false").as_bool(), "false parses");
    check_eq(std::to_string(ggmcp::Value::parse("42").as_int()), "42", "integer parses");
    check_eq(std::to_string(ggmcp::Value::parse("-7").as_int()), "-7", "negative integer parses");
    check(ggmcp::Value::parse("3.5").is_double(), "double parses");
    check(ggmcp::Value::parse("1e3").is_double(), "exponent parses as double");
    check(ggmcp::Value::parse("  \t\n 12 \r\n").is_int(), "surrounding whitespace is skipped");

    // An integer too wide for int64 degrades to a double rather than wrapping.
    const ggmcp::Value huge = ggmcp::Value::parse("99999999999999999999");
    check(huge.is_double(), "out-of-range integer falls back to double");
}

void test_json_strings() {
    group("json/strings");

    check_eq(ggmcp::Value::parse("\"plain\"").as_string(), "plain", "plain string");
    check_eq(ggmcp::Value::parse("\"a\\nb\"").as_string(), "a\nb", "escape n");
    check_eq(ggmcp::Value::parse("\"a\\\"b\"").as_string(), "a\"b", "escaped quote");
    check_eq(ggmcp::Value::parse("\"a\\\\b\"").as_string(), "a\\b", "escaped backslash");
    check_eq(ggmcp::Value::parse("\"\\u0041\"").as_string(), "A", "\\u escape");

    // U+1F600 GRINNING FACE, written as a surrogate pair, must become 4-byte UTF-8.
    const std::string emoji = ggmcp::Value::parse("\"\\ud83d\\ude00\"").as_string();
    const std::string want_emoji = "\xF0\x9F\x98\x80";
    check_eq(emoji, want_emoji, "\\uXXXX surrogate pair decodes to UTF-8");

    // Raw UTF-8 in must survive, and raw UTF-8 out must not be escaped away.
    const std::string text = "\"caf\xC3\xA9\"";
    check_eq(ggmcp::Value::parse(text).as_string(), "caf\xC3\xA9", "raw UTF-8 input decodes");
    check_eq(ggmcp::Value::make_string("caf\xC3\xA9").dump(), "\"caf\xC3\xA9\"",
             "raw UTF-8 output is not escaped");

    // Round trip through dump/parse.
    const ggmcp::Value original = ggmcp::Value::parse("[\"a\\tb\",1.5,{\"k\":[true,null]}]");
    const ggmcp::Value reparsed = ggmcp::Value::parse(original.dump());
    check(reparsed.is_array() && reparsed.size() == 3, "array round trip");
    check_eq(reparsed.at(0)->as_string(), "a\tb", "nested string round trip");
    check(reparsed.at(2)->find("k")->at(1)->is_null(), "nested null round trip");
}

void test_json_containers() {
    group("json/containers");

    const ggmcp::Value empty_array = ggmcp::Value::parse("[]");
    check(empty_array.is_array() && empty_array.size() == 0, "empty array");

    const ggmcp::Value empty_object = ggmcp::Value::parse("{}");
    check(empty_object.is_object() && empty_object.size() == 0, "empty object");

    const ggmcp::Value object = ggmcp::Value::parse("{\"b\":1,\"a\":2}");
    check(object.is_object() && object.size() == 2, "object with two keys");
    check_eq(object.dump(), "{\"a\":2,\"b\":1}", "object keys serialize in sorted order");

    // Sorted keys make server responses byte-stable, which makes tests easy.
    check_eq(ggmcp::Value::parse("{\"x\":\"\\u00e9\"}").dump(), "{\"x\":\"\xC3\xA9\"}", "dump emits UTF-8");

    // Control characters must be escaped on output to stay valid JSON.
    check_eq(ggmcp::Value::make_string(std::string("a") + static_cast<char>(0x01) + "b").dump(),
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
        const bool ok = ggmcp::Value::try_parse(text, value, error);
        check(!ok, "rejects \"" + text + "\"");
    }

}

void test_json_depth_limit() {
    group("json/depth");
    ggmcp::Value value;
    std::string error;
    const std::string deep(200, '[');
    check(!ggmcp::Value::try_parse(deep, value, error), "deeply nested arrays are rejected");
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
    check(ggmcp::Value::try_parse(reply.body, message, error), "initialize reply is valid JSON");

    const ggmcp::Value* result = message.find("result");
    check(result != nullptr, "initialize returns a result member");
    if (!result) return;

    check_eq(result->find("protocolVersion")->as_string(), "2025-06-18", "echoes a supported version");
    check(result->find("capabilities")->contains("tools"), "advertises the tools capability");
    check_eq(result->find("serverInfo")->find("name")->as_string(), "test-server", "reports serverInfo.name");
    check_eq(std::to_string(message.find("id")->as_int()), "1", "echoes the request id");
    check(!message.contains("error"), "initialize has no error member");

    // An unsupported version must fall back to the newest one we speak.
    ggmcp::Context other = http_context();
    const ggmcp::Reply fallback = dispatcher.handle(initialize_body("1999-01-01"), other);
    ggmcp::Value fallback_message;
    check(ggmcp::Value::try_parse(fallback.body, fallback_message, error), "fallback reply parses");
    check_eq(fallback_message.find("result")->find("protocolVersion")->as_string(),
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
    check(ggmcp::Value::try_parse(reply.body, message, error), "tools/list reply parses");

    const ggmcp::Value* tools = message.find("result")->find("tools");
    check(tools && tools->is_array(), "tools is an array");
    if (!tools) return;
    check_eq(std::to_string(tools->size()), "4", "four demo tools are listed");

    for (std::size_t i = 0; i < tools->size(); ++i) {
        const ggmcp::Value* tool = tools->at(i);
        check(tool->find("name") && tool->find("name")->is_string(), "tool has a name");
        check(tool->find("description") && tool->find("description")->is_string(), "tool has a description");
        const ggmcp::Value* schema = tool->find("inputSchema");
        check(schema && schema->is_object(), "tool has an inputSchema object");
        if (schema) {
            check(schema->find("type") && schema->find("type")->as_string() == "object",
                  "inputSchema.type is object");
            check(schema->find("properties") && schema->find("properties")->is_object(),
                  "inputSchema has properties");
            check(schema->find("required") && schema->find("required")->is_array(),
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
    check(ggmcp::Value::try_parse(hello.body, message, error), "hello reply parses");
    const ggmcp::Value* content = message.find("result")->find("content");
    check(content && content->is_array() && content->size() == 1, "hello returns one content block");
    check_eq(content->at(0)->find("type")->as_string(), "text", "content block type is text");
    check_has(content->at(0)->find("text")->as_string(), "Hello, world!", "hello greets the world");
    check(!message.find("result")->find("isError")->as_bool(), "hello is not an error");

    // hello with a name and loud=true.
    const ggmcp::Reply loud = dispatcher.handle(
        "{\"jsonrpc\":\"2.0\",\"id\":4,\"method\":\"tools/call\",\"params\":{\"name\":\"hello\","
        "\"arguments\":{\"name\":\"Ada Lovelace\",\"loud\":true}}}",
        http_context(session));
    check(ggmcp::Value::try_parse(loud.body, message, error), "loud reply parses");
    const std::string loud_text = message.find("result")->find("content")->at(0)->find("text")->as_string();
    check_has(loud_text, "HELLO, ADA LOVELACE!", "loud greets in upper case");
    check_has(loud_text, "http", "the greeting names the transport");

    // Non-ASCII travels through the whole stack.
    const ggmcp::Reply unicode = dispatcher.handle(
        "{\"jsonrpc\":\"2.0\",\"id\":5,\"method\":\"tools/call\",\"params\":{\"name\":\"echo\","
        "\"arguments\":{\"message\":\"caf\\u00e9 \\ud83d\\ude80\"}}}",
        http_context(session));
    check(ggmcp::Value::try_parse(unicode.body, message, error), "unicode echo reply parses");
    check_eq(message.find("result")->find("content")->at(0)->find("text")->as_string(),
             "caf\xC3\xA9 \xF0\x9F\x9A\x80", "echo round-trips non-ASCII");

    // A missing required argument is a tool-level error, not a protocol error.
    const ggmcp::Reply missing = dispatcher.handle(
        "{\"jsonrpc\":\"2.0\",\"id\":6,\"method\":\"tools/call\",\"params\":{\"name\":\"echo\","
        "\"arguments\":{}}}",
        http_context(session));
    check(ggmcp::Value::try_parse(missing.body, message, error), "missing-argument reply parses");
    check(message.find("result") != nullptr, "missing argument still returns a result");
    check(message.find("result")->find("isError")->as_bool(), "missing argument sets isError");

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

    if (g_failures != 0) {
        std::fprintf(stderr, "\n%d of %d checks FAILED\n", g_failures, g_checks);
        return 1;
    }
    std::printf("all %d checks passed\n", g_checks);
    return 0;
}
