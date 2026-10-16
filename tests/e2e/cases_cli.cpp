#include <unistd.h>

#include <cstdlib>
#include <string>
#include <vector>

#include "mcp.hpp"

using namespace e2e;

namespace {

void refuses(const std::vector<std::string>& args, const std::string& because) {
    Server server(free_port(), args, false, false, true);
    int code = -1;
    if (!server.exited(&code, 8000)) {
        throw Failure("ggmcp " + args.front() + " did not stop");
    }
    if (code != 2) {
        throw Failure("ggmcp " + args.front() + " exited " + std::to_string(code) + ", not 2\n" +
                      server.log());
    }
    CHECK_HAS(server.stderr_log(), "ggmcp:");
    CHECK_HAS(server.stderr_log(), because);
}

Json stdio_request(Server& server, const Json& body, long long id) {
    (void)id;
    server.send_line(body.dump());
    return Json::parse(server.read_line(8000));
}

Json stdio_raw(Server& server, const std::string& line) {
    server.send_line(line);
    return Json::parse(server.read_line(8000));
}

Json text_block(const Json& answer) {
    return Json::parse(answer["result"]["content"][0]["text"].get<std::string>());
}

}  // namespace

E2E_CASE(flag_validation) {
    refuses({"--transport", "carrier"}, "--transport");
    refuses({"--session-policy", "loose"}, "--session-policy");
    refuses({"--port", "not-a-port"}, "--port");
    refuses({"--port", "0"}, "--port");
    refuses({"--port", "70000"}, "--port");
    refuses({"--port"}, "needs a value");
    refuses({"--endpoint", "mcp"}, "--endpoint");
    refuses({"--endpoint", "/mcp/"}, "--endpoint");
    refuses({"--fs-root", "/no/such/directory/here"}, "--fs-root");
    refuses({"--fs-root", "/etc/hosts"}, "--fs-root");
    refuses({"--max-body", "nope"}, "--max-body");
    refuses({"--alias", "has space"}, "--alias");
    refuses({"--allow-origin"}, "--allow-origin");
    refuses({"--name", ""}, "--name");
    refuses({"--title", "two\nlines"}, "--title");
    refuses({"--host", ""}, "--host");
    refuses({"--no-such-flag"}, "unknown flag");
    refuses({"extra-argument"}, "unexpected argument");
}

E2E_CASE(help_and_version) {
    Server help(free_port(), {"--help"}, false, false);
    int code = -1;
    CHECK(help.exited(&code, 8000));
    CHECK_EQ(std::to_string(code), "0");
    const std::string text = help.log();
    CHECK_HAS(text, "Usage: ggmcp");
    CHECK_HAS(text, "--session-policy");
    CHECK_HAS(text, "default: 8081");
    CHECK_HAS(text, "default: /mcp");
    CHECK_HAS(text, "default: lenient");
    CHECK_HAS(text, "--allow-outside-root");
    CHECK_HAS(text, "--token-stdin");
    CHECK_HAS(text, "--no-cors");

    Server short_help(free_port(), {"-h"}, false, false);
    int short_code = -1;
    CHECK(short_help.exited(&short_code, 8000));
    CHECK_EQ(std::to_string(short_code), "0");

    Server version(free_port(), {"--version"}, false, false);
    int version_code = -1;
    CHECK(version.exited(&version_code, 8000));
    CHECK_EQ(std::to_string(version_code), "0");
    CHECK_HAS(version.log(), "ggmcp 0.");
}

E2E_CASE(an_unusable_root_is_fatal) {
    // A root that is not there is refused before anything is served.
    Server server(free_port(), {"--fs-root", "/no/such/directory/here"}, false, false);
    int code = -1;
    CHECK(server.exited(&code, 8000));
    CHECK_EQ(std::to_string(code), "2");
    CHECK_HAS(server.stderr_log(), "/no/such/directory/here");
}

E2E_CASE(a_signal_stops_the_process) {
    // Ctrl-C or a plain kill closes the listener and leaves 0 behind, on both
    // transports, and the stdio loop notices even with its terminal still open.
    Server http(free_port(), {"--quiet"});
    CHECK_STATUS(http.request(get("/health")), 200);
    http.send_signal(SIGTERM);
    int http_code = -1;
    CHECK(http.exited(&http_code, 5000));
    CHECK_EQ(std::to_string(http_code), "0");

    Server stdio(free_port(), {}, true);
    stdio.send_line("{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"ping\"}");
    CHECK(!Json::parse(stdio.read_line(8000)).empty());
    stdio.send_signal(SIGINT);
    int stdio_code = -1;
    CHECK(stdio.exited(&stdio_code, 5000));
    CHECK_EQ(std::to_string(stdio_code), "0");
}

E2E_CASE(a_restart_over_a_used_socket_works) {
    // A listening socket leaves accepted connections behind in TIME_WAIT. Binding
    // the same port again must still work, and a port that is still serving must
    // not be taken at all.
    const int port = free_port();
    {
        Server first(port, {"--quiet"});
        Connection connection = first.connect();
        Call health = get("/health");
        health.connection = "keep-alive";
        CHECK_STATUS(connection.request(health), 200);
        connection.close();
    }

    Server again(port, {"--quiet"});
    CHECK_STATUS(again.request(get("/health")), 200);

    // and while it serves, a second instance on that port is refused
    Server third(port, {"--quiet"}, false, false);
    int code = -1;
    CHECK(third.exited(&code, 8000));
    CHECK_EQ(std::to_string(code), "2");
    CHECK_HAS(third.stderr_log(), "cannot bind");
}

E2E_CASE(stdio_transport) {
    TempDir root;
    Server server(free_port(), {"--fs-root", root.path()}, true);

    // The banner goes where it cannot be mistaken for a message.
    CHECK_HAS(server.stderr_log(), "transport  : stdio");
    CHECK_HAS(server.stderr_log(), root.path());
    CHECK_EQ(server.stdout_log(), std::string(""));

    Json answer = stdio_request(server, envelope("initialize", init_params(), 1), 1);
    CHECK_EQ(answer["jsonrpc"].get<std::string>(), "2.0");
    CHECK_EQ(answer["id"].get<int>(), 1);
    CHECK_EQ(answer["result"]["protocolVersion"].get<std::string>(), "2025-06-18");
    CHECK_EQ(answer["result"]["serverInfo"]["name"].get<std::string>(), "ggmcp");

    // No session handling on this transport: nothing to carry, nothing to refuse.
    const Json tools = stdio_request(server, envelope("tools/list", Json(), 2), 2);
    CHECK_EQ(tools["result"]["tools"].size(), 8u);

    // A notification is answered with nothing at all: the next line is the next answer.
    server.send_line(envelope("notifications/initialized", Json(), -1).dump());
    server.send_line(envelope("ping", Json(), 3).dump());
    const Json ping = Json::parse(server.read_line(8000));
    CHECK_EQ(ping["id"].get<int>(), 3);  // the notification was answered with nothing

    Json message;
    message["message"] = "over the pipe";
    const Json echo = stdio_request(server, envelope("tools/call", tool_call("mcp-echo", message), 5), 5);
    CHECK_EQ(echo["result"]["content"][0]["text"].get<std::string>(), "over the pipe");
    CHECK_EQ(echo["result"]["isError"].get<bool>(), false);

    const Json who = text_block(stdio_request(server,
                                              envelope("tools/call", tool_call("mcp-whoami", Json::object()), 6),
                                              6));
    CHECK_EQ(who["transport"].get<std::string>(), "stdio");
    CHECK(who["session"].is_null());
    CHECK_EQ(who["root"].get<std::string>(), root.path());

    Json content;
    content["content"] = "written over stdio\n";
    content["path"] = "stdio.txt";
    stdio_request(server, envelope("tools/call", tool_call("mcp-write", content), 7), 7);
    CHECK_EQ(root.read("stdio.txt"), "written over stdio\n");

    // The same errors as over HTTP.
    CHECK_EQ(stdio_raw(server, "{not json")["error"]["code"].get<int>(), -32700);
    CHECK_EQ(stdio_raw(server, "[1,2]")["error"]["code"].get<int>(), -32600);
    CHECK_EQ(stdio_raw(server, envelope("no/such", Json(), 8).dump())["error"]["code"].get<int>(),
             -32601);

    // End of input closes it.
    server.close_stdin();
    int code = -1;
    CHECK(server.exited(&code, 8000));
    CHECK_EQ(std::to_string(code), "0");
}

E2E_CASE(stdio_ignores_the_session_policy) {
    TempDir root;
    Server server(free_port(), {"--fs-root", root.path(), "--session-policy", "strict"}, true);

    server.send_line(envelope("initialize", init_params(), 1).dump());
    const Json opened = Json::parse(server.read_line(8000));
    CHECK_EQ(opened["result"]["protocolVersion"].get<std::string>(), "2025-06-18");

    server.send_line(envelope("tools/list", Json(), 2).dump());
    const Json tools = Json::parse(server.read_line(8000));
    CHECK_EQ(tools["result"]["tools"].size(), 8u);
    CHECK(!tools["result"]["tools"].empty());
}

E2E_CASE(stdio_quiet) {
    TempDir root;
    Server server(free_port(), {"--fs-root", root.path(), "--quiet"}, true);
    server.send_line(envelope("ping", Json(), 1).dump());
    CHECK_EQ(Json::parse(server.read_line(8000))["id"].get<int>(), 1);
    CHECK_EQ(server.stderr_log(), std::string(""));
}
