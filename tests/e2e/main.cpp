// End to end tests. Every test starts a real ggmcp process and drives it over a
// socket (or through stdio), so nothing is stubbed out.

#include <algorithm>
#include <cstdio>
#include <fstream>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include <unistd.h>

#include "gg/json.hpp"
#include "harness.hpp"

namespace {

using ggt::Reply;
using ggt::RequestOptions;

const std::vector<std::string> kCoreTools = {"echo", "hello", "utc_now", "whoami"};
const std::vector<std::string> kFileTools = {"edit", "read", "write"};
const std::vector<std::string> kAllTools = {"bash",  "echo", "edit",   "hello",
                                            "read", "utc_now", "whoami", "write"};

std::string request(const std::string& id, const std::string& method,
                    const std::string& params = {}) {
    std::string out = R"({"jsonrpc":"2.0","id":)" + id + R"(,"method":")" + method + "\"";
    if (!params.empty()) {
        out += R"(,"params":)" + params;
    }
    out += "}";
    return out;
}

std::string notification(const std::string& method, const std::string& params = {}) {
    std::string out = R"({"jsonrpc":"2.0","method":")" + method + "\"";
    if (!params.empty()) {
        out += R"(,"params":)" + params;
    }
    out += "}";
    return out;
}

std::string callToolBody(const std::string& tool, const std::string& arguments) {
    return request("7", "tools/call",
                   R"({"name":")" + tool + R"(","arguments":)" + arguments + "}");
}

RequestOptions headers(std::vector<std::string> extra) {
    RequestOptions options;
    options.headers = std::move(extra);
    return options;
}

RequestOptions session(const std::string& sessionId) {
    return headers({ggt::withSession(sessionId)});
}

std::string initialize(int port, const std::string& version = "2025-06-18") {
    const Reply reply = ggt::postJson(
        port, request("1", "initialize",
                      R"({"protocolVersion":")" + version +
                          R"(","clientInfo":{"name":"e2e","version":"1"}})"));
    EXPECT(reply.status == 200, reply.body);
    return reply.header("Mcp-Session-Id");
}

std::string firstText(const Reply& reply) {
    const gg::Json body = reply.json();
    return body.at("result").at("content")[0].at("text").asString();
}

bool toolFailed(const Reply& reply) {
    return reply.json().at("result").at("isError").asBool(false);
}

std::vector<std::string> toolNames(const Reply& reply) {
    std::vector<std::string> names;
    for (const auto& tool : reply.json().at("result").at("tools").items()) {
        names.push_back(tool.at("name").asString());
    }
    return names;
}

// The first text block of a stdio reply line.
std::string lineText(const std::string& line) {
    return gg::Json::parse(line).at("result").at("content")[0].at("text").asString();
}

std::string joined(const std::vector<std::string>& values) {
    std::string out;
    for (const auto& value : values) {
        if (!out.empty()) {
            out += ", ";
        }
        out += value;
    }
    return out;
}

bool writeFile(const std::string& path, const std::string& text) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out << text;
    return out.good();
}

bool containsAll(const std::vector<std::string>& haystack, const std::vector<std::string>& needles) {
    for (const auto& needle : needles) {
        if (std::find(haystack.begin(), haystack.end(), needle) == haystack.end()) {
            return false;
        }
    }
    return true;
}

// ---------------------------------------------------------------- transports

void testHealthAndIndex() {
    ggt::Server server({"--fs-root", ggt::tempRoot("health")});

    const Reply health = ggt::get(server.port(), "/health");
    EXPECT(health.status == 200, health.body);
    EXPECT(health.json().at("status").asString() == "ok", health.body);
    EXPECT(health.json().at("endpoint").asString() == "/mcp", health.body);
    EXPECT(health.json().at("policy").asString() == "lenient", health.body);
    EXPECT(health.bodyHas("uptimeSeconds"), health.body);

    const Reply index = ggt::get(server.port(), "/");
    EXPECT(index.status == 200, index.body);
    EXPECT(index.header("Content-Type").find("text/html") != std::string::npos,
           index.header("Content-Type"));
    EXPECT(index.bodyHas("POST /mcp"), index.body);
    EXPECT(index.bodyHas("bash"), index.body);

    const Reply missing = ggt::get(server.port(), "/nope");
    EXPECT(missing.status == 404, missing.body);

    const Reply wrongMethod = ggt::get(server.port(), "/mcp");
    EXPECT(wrongMethod.status == 405, wrongMethod.body);
    EXPECT(wrongMethod.header("Allow").find("POST") != std::string::npos,
           wrongMethod.header("Allow"));
    EXPECT(wrongMethod.header("Allow").find("DELETE") != std::string::npos,
           wrongMethod.header("Allow"));
}

void testRequestsBeforeHandshake() {
    ggt::Server server({"--fs-root", ggt::tempRoot("handshake")});

    const Reply listed = ggt::postJson(server.port(), request("1", "tools/list"));
    EXPECT(listed.status == 200, listed.body);
    EXPECT(listed.header("Mcp-Session-Id").empty(), listed.header("Mcp-Session-Id"));
    EXPECT(containsAll(toolNames(listed), kCoreTools), joined(toolNames(listed)));

    const Reply ping = ggt::postJson(server.port(), request("2", "ping"));
    EXPECT(ping.status == 200, ping.body);
    EXPECT(ping.bodyHas(R"("result":{})"), ping.body);

    const Reply called = ggt::postJson(
        server.port(), request("3", "tools/call", R"({"name":"hello","arguments":{}})"));
    EXPECT(called.status == 200, called.body);
    EXPECT(firstText(called) == "Hello, world!", firstText(called));
}

void testInitializeAndSessionId() {
    ggt::Server server({"--fs-root", ggt::tempRoot("initialize")});

    for (const auto& version : {"2024-11-05", "2025-03-26", "2025-06-18"}) {
        const Reply reply = ggt::postJson(
            server.port(), request("1", "initialize",
                                   R"({"protocolVersion":")" + std::string(version) +
                                       R"(","clientInfo":{"name":"e2e","version":"9"}})"));
        EXPECT(reply.status == 200, reply.body);
        EXPECT(reply.json().at("result").at("protocolVersion").asString() == version, reply.body);
        EXPECT(!reply.header("Mcp-Session-Id").empty(), "initialize issues a session id");
        EXPECT(reply.json().at("result").at("capabilities").at("tools").contains("listChanged"),
               reply.body);
        EXPECT(reply.json().at("result").at("serverInfo").at("name").asString() == "ggmcp",
               reply.body);
    }

    const Reply odd = ggt::postJson(
        server.port(), request("1", "initialize",
                               R"({"protocolVersion":"1999-01-01","clientInfo":{"name":"e2e"}})"));
    EXPECT(odd.json().at("result").at("protocolVersion").asString() == "2025-06-18", odd.body);

    const Reply first = ggt::postJson(
        server.port(), request("1", "initialize",
                               R"({"protocolVersion":"2025-06-18","clientInfo":{"name":"e2e"}})"));
    const Reply second = ggt::postJson(
        server.port(), request("2", "initialize",
                               R"({"protocolVersion":"2025-06-18","clientInfo":{"name":"e2e"}})"));
    EXPECT(first.header("Mcp-Session-Id") != second.header("Mcp-Session-Id"),
           "session ids are unique");
    EXPECT(first.header("Mcp-Session-Id").size() >= 16, first.header("Mcp-Session-Id"));

    const Reply unknownSession = ggt::postJson(
        server.port(), request("3", "ping"), session("not-a-session"));
    EXPECT(unknownSession.status == 404, unknownSession.body);
    EXPECT(unknownSession.bodyHas("session_not_found"), unknownSession.body);

    const Reply withSessionOnInitialize = ggt::postJson(
        server.port(), request("4", "initialize", R"({"protocolVersion":"2025-06-18"})"),
        session(first.header("Mcp-Session-Id")));
    EXPECT(withSessionOnInitialize.status == 400, withSessionOnInitialize.body);
}

void testSessionPolicies() {
    const std::string root = ggt::tempRoot("policy");

    {
        ggt::Server lenient({"--fs-root", root});
        const Reply ping = ggt::postJson(lenient.port(), request("1", "ping"));
        EXPECT(ping.status == 200, ping.body);
        const Reply whoami = ggt::postJson(
            lenient.port(), callToolBody("whoami", "{}"), session("ignored-by-nothing"));
        EXPECT(whoami.status == 404, whoami.body);
    }

    {
        ggt::Server strict({"--fs-root", root, "--session-policy", "strict"});
        const Reply early = ggt::postJson(strict.port(), request("1", "ping"));
        EXPECT(early.status == 400, early.body);
        EXPECT(early.bodyHas("session_required"), early.body);

        const std::string sessionId = initialize(strict.port());
        EXPECT(!sessionId.empty(), "strict issues a session id");

        const Reply beforeHandshake =
            ggt::postJson(strict.port(), request("2", "ping"), session(sessionId));
        EXPECT(beforeHandshake.status == 400, beforeHandshake.body);
        EXPECT(beforeHandshake.bodyHas("session_not_initialized"), beforeHandshake.body);

        const Reply initialized =
            ggt::postJson(strict.port(), notification("notifications/initialized"),
                          session(sessionId));
        EXPECT(initialized.status == 202, initialized.body);
        EXPECT(initialized.body.empty(), initialized.body);

        const Reply after = ggt::postJson(strict.port(), request("3", "ping"), session(sessionId));
        EXPECT(after.status == 200, after.body);
        EXPECT(after.bodyHas(R"("result":{})"), after.body);

        const Reply stale = ggt::postJson(strict.port(), request("4", "ping"), session("nope"));
        EXPECT(stale.status == 404, stale.body);
    }

    {
        ggt::Server stateless({"--fs-root", root, "--session-policy", "stateless"});
        const Reply init = ggt::postJson(
            stateless.port(), request("1", "initialize",
                                      R"({"protocolVersion":"2025-06-18","clientInfo":{"name":"e"}})"));
        EXPECT(init.status == 200, init.body);
        EXPECT(init.header("Mcp-Session-Id").empty(), "stateless issues no session id");

        const Reply ping = ggt::postJson(stateless.port(), request("2", "ping"));
        EXPECT(ping.status == 200, ping.body);

        const Reply whoami =
            ggt::postJson(stateless.port(), callToolBody("whoami", "{}"));
        EXPECT(firstText(whoami).find("stateless") != std::string::npos, firstText(whoami));

        const Reply teardown = ggt::remove(stateless.port(), "/mcp");
        EXPECT(teardown.status == 405, teardown.body);
    }
}

void testSessionTeardown() {
    ggt::Server server({"--fs-root", ggt::tempRoot("teardown")});
    const std::string sessionId = initialize(server.port());
    EXPECT(!sessionId.empty(), "the handshake issued a session");

    const Reply close = ggt::remove(server.port(), "/mcp", session(sessionId));
    EXPECT(close.status == 200, close.body);
    EXPECT(close.bodyHas("session closed"), close.body);

    const Reply after = ggt::postJson(server.port(), request("9", "ping"), session(sessionId));
    EXPECT(after.status == 404, after.body);

    const Reply closeAgain = ggt::remove(server.port(), "/mcp", session(sessionId));
    EXPECT(closeAgain.status == 404, closeAgain.body);

    const Reply noSession = ggt::remove(server.port(), "/mcp");
    EXPECT(noSession.status == 400, noSession.body);
}

void testKeepAliveAndChunkedBodies() {
    ggt::Server server({"--fs-root", ggt::tempRoot("keepalive")});

    ggt::HttpClient client;
    std::string error;
    EXPECT(client.open("127.0.0.1", server.port(), error), error);

    Reply health;
    EXPECT(client.request("GET", "/health", {}, RequestOptions(), health, error), error);
    EXPECT(health.status == 200, health.body);
    EXPECT(client.isOpen(), "the connection survived the first reply");

    Reply ping;
    EXPECT(client.request("POST", "/mcp", request("1", "ping"), RequestOptions(), ping, error),
           error);
    EXPECT(ping.status == 200, ping.body);
    EXPECT(client.isOpen(), "the connection is kept alive between requests");

    Reply listed;
    EXPECT(client.request("POST", "/mcp", request("2", "tools/list"), RequestOptions(), listed,
                          error),
           error);
    EXPECT(listed.status == 200, listed.body);

    RequestOptions chunked;
    chunked.chunked = true;
    chunked.headers = {"Content-Type: application/json", "Accept: application/json"};
    chunked.headers.push_back(ggt::withSession(initialize(server.port())));
    Reply chunkedReply;
    EXPECT(client.request("POST", "/mcp", callToolBody("echo", R"({"message":"chunked"})"),
                          chunked, chunkedReply, error),
           error);
    EXPECT(chunkedReply.status == 200, chunkedReply.body);
    EXPECT(firstText(chunkedReply) == "chunked", firstText(chunkedReply));

    RequestOptions closed;
    closed.closeConnection = true;
    Reply last;
    EXPECT(client.request("GET", "/health", {}, closed, last, error), error);
    EXPECT(last.status == 200, last.body);

    const Reply bare = ggt::postJson(server.port(), callToolBody("echo", R"({"message":"plain"})"),
                                     headers({"Accept: application/json"}));
    EXPECT(bare.bodyHas("plain"), bare.body);
}

void testMethodsAndErrors() {
    ggt::Server server({"--fs-root", ggt::tempRoot("methods")});

    const Reply ping = ggt::postJson(server.port(), request("1", "ping"));
    EXPECT(ping.status == 200, ping.body);
    EXPECT(ping.json().at("result").isObject(), ping.body);
    EXPECT(ping.json().at("result").empty(), ping.body);

    const Reply unknown = ggt::postJson(server.port(), request("2", "does/not/exist"));
    EXPECT(unknown.status == 200, unknown.body);
    EXPECT(unknown.json().at("error").at("code").asInt() == -32601, unknown.body);
    EXPECT(unknown.json().at("id").asInt() == 2, unknown.body);

    const Reply broken = ggt::postJson(server.port(), "{not json");
    EXPECT(broken.status == 200, broken.body);
    EXPECT(broken.json().at("error").at("code").asInt() == -32700, broken.body);

    const Reply noMethod = ggt::postJson(server.port(), R"({"jsonrpc":"2.0","id":3})");
    EXPECT(noMethod.status == 200, noMethod.body);
    EXPECT(noMethod.json().at("error").at("code").asInt() == -32600, noMethod.body);

    const Reply badVersion = ggt::postJson(server.port(), R"({"jsonrpc":"1.0","id":4,"method":"ping"})");
    EXPECT(badVersion.json().at("error").at("code").asInt() == -32600, badVersion.body);

    const Reply note = ggt::postJson(server.port(), notification("notifications/cancelled",
                                                                 R"({"requestId":"1"})"));
    EXPECT(note.status == 202, note.body);
    EXPECT(note.body.empty(), note.body);

    const Reply resources = ggt::postJson(server.port(), request("5", "resources/list"));
    EXPECT(resources.json().at("result").at("resources").isArray(), resources.body);
    EXPECT(resources.json().at("result").at("resources").empty(), resources.body);

    const Reply prompts = ggt::postJson(server.port(), request("6", "prompts/list"));
    EXPECT(prompts.json().at("result").at("prompts").isArray(), prompts.body);
    EXPECT(prompts.json().at("result").at("prompts").empty(), prompts.body);

    const Reply acceptOnly = ggt::postJson(
        server.port(), request("7", "ping"), headers({"Accept: text/event-stream"}));
    EXPECT(acceptOnly.status == 406, acceptOnly.body);

    const Reply wrongType = ggt::postJson(server.port(), "hello",
                                          headers({"Content-Type: text/plain"}));
    EXPECT(wrongType.status == 415, wrongType.body);

    const Reply protocolHeader = ggt::postJson(
        server.port(), request("8", "ping"), headers({"MCP-Protocol-Version: 2000-01-01"}));
    EXPECT(protocolHeader.status == 400, protocolHeader.body);
    EXPECT(protocolHeader.bodyHas("unsupported_protocol_version"), protocolHeader.body);
}

// --------------------------------------------------------------------- tools

void testToolLists() {
    const std::string root = ggt::tempRoot("tools");

    {
        ggt::Server server({"--fs-root", root});
        const Reply listed = ggt::postJson(server.port(), request("1", "tools/list"));
        const std::vector<std::string> names = toolNames(listed);
        EXPECT(names == kAllTools, joined(names));
        EXPECT(listed.bodyHas("inputSchema"), listed.body);
        EXPECT(listed.bodyHas("\"required\""), listed.body);
    }

    {
        ggt::Server server({"--fs-root", root, "--no-file-tools", "--no-bash"});
        const Reply listed = ggt::postJson(server.port(), request("1", "tools/list"));
        const std::vector<std::string> names = toolNames(listed);
        EXPECT(names == kCoreTools, joined(names));

        const Reply read = ggt::postJson(server.port(), callToolBody("read", R"({"path":"x"})"));
        EXPECT(read.status == 200, read.body);
        EXPECT(read.json().at("error").at("code").asInt() == -32602, read.body);
        EXPECT(read.bodyHas("--no-file-tools"), read.body);

        const Reply bash = ggt::postJson(server.port(), callToolBody("bash", R"({"command":"true"})"));
        EXPECT(bash.json().at("error").at("code").asInt() == -32602, bash.body);
        EXPECT(bash.bodyHas("--no-bash"), bash.body);

        const Reply hello = ggt::postJson(
            server.port(), callToolBody("hello", R"({"name":"pi","loud":true})"));
        EXPECT(firstText(hello) == "HELLO, PI!", firstText(hello));

        const Reply echo = ggt::postJson(
            server.port(), callToolBody("echo", R"({"message":" a  b\nc "})"));
        EXPECT(firstText(echo) == " a  b\nc ", firstText(echo));

        const Reply missingArgument = ggt::postJson(
            server.port(), callToolBody("echo", R"({"nope":1})"));
        EXPECT(toolFailed(missingArgument), missingArgument.body);
        EXPECT(missingArgument.bodyHas("'message' is required"), missingArgument.body);
    }

    {
        ggt::Server server({"--fs-root", root, "--no-file-tools"});
        std::vector<std::string> expected;
        for (const auto& name : kAllTools) {
            if (std::find(kFileTools.begin(), kFileTools.end(), name) == kFileTools.end()) {
                expected.push_back(name);
            }
        }
        const std::vector<std::string> names =
            toolNames(ggt::postJson(server.port(), request("1", "tools/list")));
        EXPECT(names == expected, joined(names));
    }
}

void testCoreTools() {
    ggt::Server server({"--fs-root", ggt::tempRoot("core")});
    const std::string sessionId = initialize(server.port());
    ggt::postJson(server.port(), notification("notifications/initialized"), session(sessionId));

    const Reply utcNow = ggt::postJson(server.port(), callToolBody("utc_now", "{}"),
                                       session(sessionId));
    const gg::Json now = gg::Json::parse(firstText(utcNow));
    EXPECT(now.at("iso8601").asString().size() == 24, now.at("iso8601").asString());
    EXPECT(now.at("iso8601").asString().back() == 'Z', now.at("iso8601").asString());
    EXPECT(now.at("epochMillis").asInt() > 1700000000000LL, now.at("epochMillis").asString());

    const Reply whoami = ggt::postJson(server.port(), callToolBody("whoami", "{}"),
                                       session(sessionId));
    const std::string text = firstText(whoami);
    const gg::Json body = gg::Json::parse(text);
    EXPECT(body.at("session").at("id").asString() == sessionId, text);
    EXPECT(body.at("session").at("state").asString() == "initialized", text);
    EXPECT(body.at("session").at("client").at("name").asString() == "e2e", text);
    EXPECT(body.at("session").at("protocolVersion").asString() == "2025-06-18", text);
    EXPECT(body.at("transport").asString() == "http", text);
    EXPECT(body.at("confined").asBool(), text);
    EXPECT(body.at("session").at("requests").asInt() >= 2, text);
}

void testFileTools() {
    const std::string root = ggt::tempRoot("files");
    ggt::Server server({"--fs-root", root});

    const std::string path = root + "/nested/hello.txt";
    const Reply written = ggt::postJson(
        server.port(), callToolBody("write",
                                    R"({"path":")" + path + R"(","content":"one\ntwo\nthree\n"})"));
    EXPECT(!toolFailed(written), firstText(written));
    EXPECT(firstText(written).find("\"created\":true") != std::string::npos, firstText(written));

    std::ifstream onDisk(path, std::ios::binary);
    std::ostringstream onDiskText;
    onDiskText << onDisk.rdbuf();
    EXPECT(onDiskText.str() == "one\ntwo\nthree\n", onDiskText.str());

    const Reply read = ggt::postJson(
        server.port(), callToolBody("read", R"({"path":")" + path + R"(","offset":2,"limit":2})"));
    EXPECT(firstText(read) == "two\nthree\n", firstText(read));

    const Reply readAll =
        ggt::postJson(server.port(), callToolBody("read", R"({"path":")" + path + R"("})"));
    EXPECT(firstText(readAll) == "one\ntwo\nthree\n", firstText(readAll));

    const Reply readPastEnd =
        ggt::postJson(server.port(), callToolBody("read", R"({"path":")" + path + R"(","offset":99})"));
    EXPECT(toolFailed(readPastEnd), readPastEnd.body);
    EXPECT(readPastEnd.bodyHas("past the end"), readPastEnd.body);

    const Reply appended = ggt::postJson(
        server.port(), callToolBody("write",
                                    R"({"path":")" + path + R"(","content":"four\n","append":true})"));
    EXPECT(firstText(appended).find("\"appended\":true") != std::string::npos,
           firstText(appended));
    const Reply afterAppend =
        ggt::postJson(server.port(), callToolBody("read", R"({"path":")" + path + R"("})"));
    EXPECT(firstText(afterAppend) == "one\ntwo\nthree\nfour\n", firstText(afterAppend));

    const Reply edited = ggt::postJson(
        server.port(), callToolBody("edit",
                                    R"({"path":")" + path +
                                        R"(","edits":[{"oldText":"two","newText":"TWO"},{"oldText":"four","newText":"FOUR"}]})"));
    EXPECT(!toolFailed(edited), firstText(edited));
    const Reply afterEdit =
        ggt::postJson(server.port(), callToolBody("read", R"({"path":")" + path + R"("})"));
    EXPECT(firstText(afterEdit) == "one\nTWO\nthree\nFOUR\n", firstText(afterEdit));

    const Reply dryRun = ggt::postJson(
        server.port(), callToolBody("edit",
                                    R"({"path":")" + path +
                                        R"(","dryRun":true,"edits":[{"oldText":"one","newText":"nope"}]})"));
    EXPECT(firstText(dryRun).find("\"dryRun\":true") != std::string::npos, firstText(dryRun));
    const Reply afterDryRun =
        ggt::postJson(server.port(), callToolBody("read", R"({"path":")" + path + R"("})"));
    EXPECT(firstText(afterDryRun) == "one\nTWO\nthree\nFOUR\n", firstText(afterDryRun));

    // all or nothing: the second edit misses, so the first one must not stick
    const Reply partial = ggt::postJson(
        server.port(), callToolBody("edit",
                                    R"({"path":")" + path +
                                        R"(","edits":[{"oldText":"one","newText":"ONE"},{"oldText":"absent","newText":"x"}]})"));
    EXPECT(toolFailed(partial), partial.body);
    EXPECT(partial.bodyHas("no match"), partial.body);
    const Reply afterPartial =
        ggt::postJson(server.port(), callToolBody("read", R"({"path":")" + path + R"("})"));
    EXPECT(firstText(afterPartial) == "one\nTWO\nthree\nFOUR\n", firstText(afterPartial));

    // confinement
    const Reply outside =
        ggt::postJson(server.port(), callToolBody("read", R"({"path":"/etc/hosts"})"));
    EXPECT(toolFailed(outside), outside.body);
    EXPECT(outside.bodyHas("outside the root"), outside.body);

    const Reply escape = ggt::postJson(
        server.port(),
        callToolBody("read", R"({"path":")" + root + "/../../../../etc/hosts" + R"("})"));
    EXPECT(escape.bodyHas("outside the root"), escape.body);

    const std::string link = root + "/escape";
    if (::symlink("/etc/hosts", link.c_str()) != 0) {
        ggt::note("cannot create a symlink here, skipping the symlink check");
    } else {
        const Reply viaLink =
            ggt::postJson(server.port(), callToolBody("read", R"({"path":")" + link + R"("})"));
        EXPECT(viaLink.bodyHas("outside the root"), viaLink.body);
    }

    const Reply badEdit = ggt::postJson(
        server.port(), callToolBody("edit", R"({"path":")" + path + R"(","edits":[]})"));
    EXPECT(badEdit.bodyHas("non-empty array"), badEdit.body);
}

void testBashTool() {
    ggt::Server server({"--fs-root", ggt::tempRoot("bash")});

    const Reply echo = ggt::postJson(
        server.port(), callToolBody("bash", R"({"command":"printf 'a%sb' X"})"));
    const gg::Json body = gg::Json::parse(firstText(echo));
    EXPECT(body.at("exitCode").asInt() == 0, echo.body);
    EXPECT(body.at("output").asString() == "aXb", echo.body);
    EXPECT(!toolFailed(echo), echo.body);

    const Reply failing =
        ggt::postJson(server.port(), callToolBody("bash", R"({"command":"echo oops >&2; exit 7"})"));
    const gg::Json failingBody = gg::Json::parse(firstText(failing));
    EXPECT(failingBody.at("exitCode").asInt() == 7, failing.body);
    EXPECT(failingBody.at("error").asString() == "oops\n", failing.body);

    const Reply timedOut =
        ggt::postJson(server.port(), callToolBody("bash", R"({"command":"sleep 5","timeout":1})"));
    EXPECT(toolFailed(timedOut), timedOut.body);
    EXPECT(firstText(timedOut).find("\"timedOut\":true") != std::string::npos,
           firstText(timedOut));

    const Reply needsShell = ggt::postJson(
        server.port(), callToolBody("bash", R"({"command":"for i in 1 2 3; do printf %s $i; done"})"));
    EXPECT(gg::Json::parse(firstText(needsShell)).at("output").asString() == "123",
           needsShell.body);

    const Reply noCommand = ggt::postJson(server.port(), callToolBody("bash", "{}"));
    EXPECT(toolFailed(noCommand), noCommand.body);
}

void testOutsideRoot() {
    const std::string root = ggt::tempRoot("outsideroot");
    const std::string outside = ggt::tempRoot("outside");
    const std::string victim = outside + "/secret.txt";
    EXPECT(writeFile(victim, "top secret\n"), victim);

    ggt::Server server({"--fs-root", root, "--allow-outside-root"});
    const Reply read =
        ggt::postJson(server.port(), callToolBody("read", R"({"path":")" + victim + R"("})"));
    EXPECT(firstText(read) == "top secret\n", firstText(read));

    const Reply health = ggt::get(server.port(), "/health");
    EXPECT(health.json().at("root").asString().find("outsideroot") != std::string::npos,
           health.body);
}

// -------------------------------------------------------------------- origins

void testOriginPolicies() {
    const std::string root = ggt::tempRoot("origin");

    {
        ggt::Server server({"--fs-root", root});
        const Reply alien = ggt::postJson(server.port(), request("1", "ping"),
                                         headers({"Origin: http://evil.example"}));
        EXPECT(alien.status == 200, alien.body);
    }

    {
        ggt::Server server({"--fs-root", root, "--strict-origin"});
        const Reply alien = ggt::postJson(server.port(), request("1", "ping"),
                                         headers({"Origin: http://evil.example"}));
        EXPECT(alien.status == 403, alien.body);
        EXPECT(alien.bodyHas("not allowed"), alien.body);

        const Reply own = ggt::postJson(
            server.port(), request("2", "ping"),
            headers({"Origin: http://127.0.0.1:" + std::to_string(server.port())}));
        EXPECT(own.status == 200, own.body);

        const Reply none = ggt::postJson(server.port(), request("3", "ping"));
        EXPECT(none.status == 200, none.body);
    }

    {
        ggt::Server server({"--fs-root", root, "--allow-origin", "http://good.example"});
        const Reply good = ggt::postJson(server.port(), request("1", "ping"),
                                        headers({"Origin: http://good.example"}));
        EXPECT(good.status == 200, good.body);

        const Reply bad = ggt::postJson(server.port(), request("2", "ping"),
                                       headers({"Origin: http://bad.example"}));
        EXPECT(bad.status == 403, bad.body);

        const Reply teardown =
            ggt::remove(server.port(), "/mcp",
                        headers({"Mcp-Session-Id: x", "Origin: http://bad.example"}));
        EXPECT(teardown.status == 403, teardown.body);
    }
}

// --------------------------------------------------------------------- CORS

void testCorsFromABrowser() {
    const std::string root = ggt::tempRoot("cors");
    const std::string origin = "http://100.111.34.101:8033";
    const std::vector<std::string> preflightHeaders = {
        "Origin: " + origin,
        "Access-Control-Request-Method: POST",
        "Access-Control-Request-Headers: content-type, authorization",
    };

    {
        ggt::Server server({"--fs-root", root});

        const Reply preflight = ggt::send(server.port(), "OPTIONS", "/mcp", {},
                                          headers(preflightHeaders));
        EXPECT(preflight.status == 204, preflight.body);
        EXPECT(preflight.header("Access-Control-Allow-Origin") == origin, preflight.body);
        EXPECT(preflight.header("Access-Control-Allow-Methods").find("POST") !=
                   std::string::npos,
               preflight.header("Access-Control-Allow-Methods"));
        EXPECT(preflight.header("Access-Control-Allow-Headers") ==
                   "content-type, authorization",
               preflight.header("Access-Control-Allow-Headers"));
        EXPECT(!preflight.header("Access-Control-Max-Age").empty(),
               preflight.header("Access-Control-Max-Age"));
        EXPECT(preflight.header("Vary").find("Origin") != std::string::npos,
               preflight.header("Vary"));

        const Reply open = ggt::send(server.port(), "OPTIONS", "/mcp", {},
                                     headers({"Origin: " + origin}));
        EXPECT(open.status == 204, open.body);
        EXPECT(!open.header("Access-Control-Allow-Headers").empty(),
               open.header("Access-Control-Allow-Headers"));

        const Reply init = ggt::postJson(
            server.port(), request("1", "initialize",
                                   R"({"protocolVersion":"2025-06-18","clientInfo":{"name":"web"}})"),
            headers({"Origin: " + origin}));
        EXPECT(init.status == 200, init.body);
        EXPECT(init.header("Access-Control-Allow-Origin") == origin,
               init.header("Access-Control-Allow-Origin"));
        EXPECT(init.header("Access-Control-Expose-Headers").find("Mcp-Session-Id") !=
                   std::string::npos,
               init.header("Access-Control-Expose-Headers"));
        EXPECT(!init.header("Mcp-Session-Id").empty(), "a browser can read the session id");

        const Reply health = ggt::send(server.port(), "GET", "/health", {},
                                       headers({"Origin: " + origin}));
        EXPECT(health.header("Access-Control-Allow-Origin") == origin, health.body);

        const Reply index = ggt::send(server.port(), "GET", "/", {},
                                      headers({"Origin: " + origin}));
        EXPECT(index.header("Access-Control-Allow-Origin") == origin, index.body);

        const Reply anonymous = ggt::postJson(server.port(), request("2", "ping"));
        EXPECT(anonymous.header("Access-Control-Allow-Origin").empty(),
               "a request with no Origin gets no CORS headers");
    }

    {
        ggt::Server strict({"--fs-root", root, "--strict-origin"});
        const Reply preflight = ggt::send(strict.port(), "OPTIONS", "/mcp", {},
                                          headers(preflightHeaders));
        EXPECT(preflight.status == 403, preflight.body);
        EXPECT(preflight.header("Access-Control-Allow-Origin").empty(), preflight.body);

        const Reply posted = ggt::postJson(strict.port(), request("1", "ping"),
                                           headers({"Origin: " + origin}));
        EXPECT(posted.status == 403, posted.body);

        const Reply own = ggt::send(strict.port(), "OPTIONS", "/mcp", {},
                                    headers({"Origin: http://127.0.0.1:" +
                                             std::to_string(strict.port())}));
        EXPECT(own.status == 204, own.body);
        EXPECT(!own.header("Access-Control-Allow-Origin").empty(), own.body);
    }

    {
        ggt::Server named({"--fs-root", root, "--allow-origin", "http://good.example",
                           "--allow-origin", "*"});
        const Reply good = ggt::send(named.port(), "OPTIONS", "/mcp", {},
                                     headers({"Origin: http://good.example"}));
        EXPECT(good.status == 204, good.body);
        EXPECT(good.header("Access-Control-Allow-Origin") == "http://good.example", good.body);
    }

    {
        ggt::Server quiet({"--fs-root", root, "--strict-origin", "--allow-origin",
                           "http://good.example"});
        const Reply bad = ggt::send(quiet.port(), "OPTIONS", "/mcp", {},
                                    headers({"Origin: http://bad.example"}));
        EXPECT(bad.status == 403, bad.body);
    }

    {
        ggt::Server noCors({"--fs-root", root, "--no-cors"});
        const Reply preflight = ggt::send(noCors.port(), "OPTIONS", "/mcp", {},
                                          headers(preflightHeaders));
        EXPECT(preflight.status == 405, preflight.body);
        EXPECT(preflight.header("Access-Control-Allow-Origin").empty(), preflight.body);
        const Reply posted = ggt::postJson(noCors.port(), request("1", "ping"),
                                           headers({"Origin: " + origin}));
        EXPECT(posted.status == 200, posted.body);
        EXPECT(posted.header("Access-Control-Allow-Origin").empty(),
               posted.header("Access-Control-Allow-Origin"));
    }

    {
        ggt::Server guarded({"--fs-root", root, "--require-token", "--token-env", "GGMCP_TOKEN"},
                            {"GGMCP_TOKEN=cors-token"});
        const Reply denied = ggt::postJson(guarded.port(), request("1", "ping"),
                                          headers({"Origin: " + origin}));
        EXPECT(denied.status == 401, denied.body);
        EXPECT(denied.header("Access-Control-Allow-Origin") == origin,
               "a browser can read the 401 body");

        const Reply accepted = ggt::postJson(
            guarded.port(), request("2", "ping"),
            headers({"Origin: " + origin, "Authorization: Bearer cors-token"}));
        EXPECT(accepted.status == 200, accepted.body);
        EXPECT(accepted.header("Access-Control-Allow-Origin") == origin, accepted.body);

        const Reply metadata = ggt::send(guarded.port(), "GET",
                                         "/.well-known/oauth-protected-resource", {},
                                         headers({"Origin: " + origin}));
        EXPECT(metadata.status == 200, metadata.body);
        EXPECT(metadata.header("Access-Control-Allow-Origin") == origin, metadata.body);
    }

    const ggt::Process help = ggt::runBinary({"--help"});
    EXPECT(help.output.find("--no-cors") != std::string::npos, help.output);
}

// ---------------------------------------------------------------- authorization

void testBearerTokens() {
    const std::string root = ggt::tempRoot("token");
    const std::string tokenPath = root + "/token.txt";
    EXPECT(writeFile(tokenPath, "first-token\n"), tokenPath);

    ggt::Server server({"--fs-root", root, "--require-token", "--token-file", tokenPath});
    const std::string endpoint = "/mcp";

    const Reply anonymous = ggt::postJson(server.port(), request("1", "ping"));
    EXPECT(anonymous.status == 401, anonymous.body);
    EXPECT(anonymous.header("WWW-Authenticate").find("Bearer") != std::string::npos,
           anonymous.header("WWW-Authenticate"));
    EXPECT(anonymous.header("WWW-Authenticate").find("error=\"invalid_request\"") !=
               std::string::npos,
           anonymous.header("WWW-Authenticate"));
    EXPECT(anonymous.header("WWW-Authenticate").find("resource_metadata=\"http://") !=
               std::string::npos,
           anonymous.header("WWW-Authenticate"));
    EXPECT(anonymous.json().at("resource").asString() ==
               "http://127.0.0.1:" + std::to_string(server.port()) + endpoint,
           anonymous.body);
    EXPECT(anonymous.json().contains("authorization_servers"), anonymous.body);

    const Reply wrong = ggt::postJson(server.port(), request("2", "ping"),
                                     headers({"Authorization: Bearer nope"}));
    EXPECT(wrong.status == 401, wrong.body);
    EXPECT(wrong.header("WWW-Authenticate").find("error=\"invalid_token\"") != std::string::npos,
           wrong.header("WWW-Authenticate"));

    const Reply basic = ggt::postJson(server.port(), request("3", "ping"),
                                     headers({"Authorization: Basic Zm9vOmJhcg=="}));
    EXPECT(basic.status == 401, basic.body);

    const Reply inQuery = ggt::postJson(server.port(), request("4", "ping"),
                                       headers({"Authorization: Bearer first-token"}),
                                       "/mcp?token=first-token");
    EXPECT(inQuery.status == 401, inQuery.body);
    EXPECT(inQuery.header("WWW-Authenticate").find("query string") != std::string::npos,
           inQuery.header("WWW-Authenticate"));

    const Reply allowed = ggt::postJson(server.port(), request("5", "ping"),
                                       headers({"Authorization: Bearer first-token"}));
    EXPECT(allowed.status == 200, allowed.body);
    EXPECT(allowed.bodyHas(R"("result":{})"), allowed.body);

    const Reply health = ggt::get(server.port(), "/health");
    EXPECT(health.status == 401, health.body);
    const Reply healthWithToken =
        ggt::get(server.port(), "/health", headers({"Authorization: Bearer first-token"}));
    EXPECT(healthWithToken.status == 200, healthWithToken.body);

    const Reply index = ggt::get(server.port(), "/");
    EXPECT(index.status == 401, index.body);

    const Reply metadata = ggt::get(server.port(), "/.well-known/oauth-protected-resource");
    EXPECT(metadata.status == 200, metadata.body);
    EXPECT(metadata.json().at("resource_name").asString() == "ggmcp", metadata.body);

    // rotation without a restart
    EXPECT(writeFile(tokenPath, "second-token"), tokenPath);
    bool rotated = false;
    for (int attempt = 0; attempt < 20 && !rotated; ++attempt) {
        const Reply oldToken = ggt::postJson(server.port(), request("6", "ping"),
                                            headers({"Authorization: Bearer first-token"}));
        const Reply newToken = ggt::postJson(server.port(), request("7", "ping"),
                                            headers({"Authorization: Bearer second-token"}));
        rotated = oldToken.status == 401 && newToken.status == 200;
        if (!rotated) {
            ggt::sleepFor(250);
        }
    }
    EXPECT(rotated, "the server picked up the rotated token file");
}

void testTokenFromEnvironment() {
    ggt::Server server({"--fs-root", ggt::tempRoot("tokenenv"), "--require-token"},
                       {"GGMCP_TOKEN=env-token"});

    const Reply allowed = ggt::postJson(server.port(), request("1", "ping"),
                                       headers({"Authorization: Bearer env-token"}));
    EXPECT(allowed.status == 200, allowed.body);

    const Reply denied = ggt::postJson(server.port(), request("2", "ping"),
                                      headers({"Authorization: Bearer env-toke"}));
    EXPECT(denied.status == 401, denied.body);

    const Reply health = ggt::get(server.port(), "/health",
                                 headers({"Authorization: Bearer env-token"}));
    EXPECT(health.bodyHas("\"token\":\"env:GGMCP_TOKEN\""), health.body);
}

// ------------------------------------------------------------ flag validation

void testCommandLineValidation() {
    const std::string root = ggt::tempRoot("flags");

    struct Case {
        std::vector<std::string> args;
        std::string needle;
    };

    const std::vector<Case> cases = {
        {{"--nope"}, "unknown option"},
        {{"positional"}, "unexpected argument"},
        {{"--port"}, "needs a value"},
        {{"--port", "not-a-number"}, "needs a number"},
        {{"--port", "70000"}, "needs a number"},
        {{"--endpoint", "mcp"}, "absolute path"},
        {{"--transport", "carrier"}, "--transport needs 'http' or 'stdio'"},
        {{"--session-policy", "loose"}, "lenient, strict, stateless"},
        {{"--fs-root", root + "/does-not-exist"}, "is not a directory"},
        {{"--fs-root", "relative/path"}, "absolute path"},
        {{"--bash-timeout", "0"}, "between 1 and 3600"},
        {{"--allow-origin", "no-scheme"}, "needs a scheme"},
        {{"--require-token", "--transport", "stdio"}, "only applies to the http transport"},
        {{"--token-file", "/tmp/whatever"}, "needs --require-token"},
        {{"--require-token", "--token-file", "/tmp/whatever", "--token-env", "TOKEN"},
         "cannot both be used"},
        {{"--require-token", "--token-stdin", "--token-file", "/tmp/whatever"},
         "cannot be combined"},
        {{"--require-token"}, "no token to require"},
        {{"--require-token", "--token-file", "/tmp/ggmcp-e2e-missing-token"},
         "is not readable"},
        {{"--json-backend", "sax"}, "unknown json backend"},
        {{"--http-backend", "libev"}, "unknown http backend"},
    };

    for (const auto& testCase : cases) {
        std::vector<std::string> env = {"GGMCP_TOKEN="};
        const ggt::Process process = ggt::runBinary(testCase.args, env);
        EXPECT(process.exitCode == 2, testCase.args.front() + " -> exit " +
                                           std::to_string(process.exitCode) + " " +
                                           process.output);
        EXPECT(process.output.find(testCase.needle) != std::string::npos,
               testCase.args.front() + " -> " + process.output);
    }

    const ggt::Process help = ggt::runBinary({"--help"});
    EXPECT(help.exitCode == 0, help.output);
    EXPECT(help.output.find("usage: ggmcp") != std::string::npos, help.output);
    EXPECT(help.output.find("--session-policy") != std::string::npos, help.output);

    const ggt::Process version = ggt::runBinary({"--version"});
    EXPECT(version.exitCode == 0, version.output);
    EXPECT(version.output.find("ggmcp ") != std::string::npos, version.output);

    const ggt::Process goodFlags = ggt::runBinary({"--transport", "stdio", "--fs-root", root,
                                                   "--no-file-tools", "--no-bash", "--quiet"},
                                                 {"GGMCP_TOKEN="});
    EXPECT(goodFlags.exitCode == 0, goodFlags.output);
    EXPECT(goodFlags.output.empty(), goodFlags.output);
}

// ---------------------------------------------------------------------- stdio

void testStdioTransport() {
    const std::string root = ggt::tempRoot("stdio");
    ggt::StdioSession session({"--transport", "stdio", "--fs-root", root});

    session.send(request("1", "initialize",
                         R"({"protocolVersion":"2025-06-18","clientInfo":{"name":"stdio","version":"1"}})"));
    const gg::Json initialized = gg::Json::parse(session.receive());
    EXPECT(initialized.at("result").at("protocolVersion").asString() == "2025-06-18",
           initialized.dump());

    session.send(notification("notifications/initialized"));
    EXPECT(session.expectNoReply(), "a notification gets no reply");

    session.send(request("2", "ping"));
    EXPECT(session.receive().find(R"("result":{})") != std::string::npos, "ping over stdio");

    session.send(request("3", "tools/list"));
    const gg::Json listed = gg::Json::parse(session.receive());
    std::vector<std::string> names;
    for (const auto& tool : listed.at("result").at("tools").items()) {
        names.push_back(tool.at("name").asString());
    }
    EXPECT(names == kAllTools, joined(names));

    session.send(request("4", "tools/call",
                         R"({"name":"whoami","arguments":{}})"));
    const gg::Json whoami = gg::Json::parse(session.receive());
    const std::string text =
        whoami.at("result").at("content")[0].at("text").asString();
    EXPECT(text.find("\"transport\":\"stdio\"") != std::string::npos, text);
    EXPECT(text.find("\"stateless\"") != std::string::npos, text);

    session.send(request("5", "tools/call",
                         R"({"name":"write","arguments":{"path":"stdio.txt","content":"from stdio\n"}})"));
    const std::string written = lineText(session.receive());
    EXPECT(written.find("\"created\":true") != std::string::npos, written);

    session.send(request("6", "tools/call",
                         R"({"name":"read","arguments":{"path":"stdio.txt"}})"));
    EXPECT(lineText(session.receive()) == "from stdio\n", "read over stdio");

    session.send(request("7", "unknown/method"));
    EXPECT(session.receive().find("-32601") != std::string::npos, "unknown method over stdio");

    session.send("{oops");
    EXPECT(session.receive().find("-32700") != std::string::npos, "parse error over stdio");

    session.closeInput();
    EXPECT(session.wait() == 0, "the stdio server exits cleanly on end of input");
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: %s <path to ggmcp> [test name filter]\n", argv[0]);
        return 2;
    }
    ggt::setBinary(argv[1]);

    return ggt::runAll(
        {
            {"health and index page", testHealthAndIndex},
            {"requests before the handshake", testRequestsBeforeHandshake},
            {"initialize and the session id", testInitializeAndSessionId},
            {"session policies", testSessionPolicies},
            {"session teardown with DELETE", testSessionTeardown},
            {"keep-alive and chunked bodies", testKeepAliveAndChunkedBodies},
            {"methods, ping and json-rpc errors", testMethodsAndErrors},
            {"tools offered by default and when disabled", testToolLists},
            {"core tools", testCoreTools},
            {"file tools against a temporary root", testFileTools},
            {"bash tool", testBashTool},
            {"--allow-outside-root", testOutsideRoot},
            {"origin policies", testOriginPolicies},
            {"cors from a browser", testCorsFromABrowser},
            {"bearer tokens", testBearerTokens},
            {"bearer token from the environment", testTokenFromEnvironment},
            {"command line flag validation", testCommandLineValidation},
            {"stdio transport", testStdioTransport},
        },
        argc - 1, argv + 1);
}
