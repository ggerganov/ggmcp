#include <string>
#include <vector>

#include "mcp.hpp"

using namespace e2e;

namespace {

Json tools_list() { return envelope("tools/list", Json(), 2); }

}  // namespace

E2E_CASE(initialize) {
    Server server(free_port(), {"--quiet"});

    const std::vector<std::string> spoken = {"2024-11-05", "2025-03-26", "2025-06-18"};
    for (const std::string& version : spoken) {
        Message answer = server.request(rpc("/mcp", envelope("initialize", init_params(version), 1)));
        CHECK_STATUS(answer, 200);
        CHECK_EQ(answer.json()["result"]["protocolVersion"].get<std::string>(), version);
        CHECK(answer.header("Mcp-Session-Id").size() == 40);
        server.request(rpc("/mcp", envelope("notifications/initialized", Json(), -1),
                           answer.header("Mcp-Session-Id")));
    }

    // A version this server does not speak is answered with the newest one it does.
    Message odd = server.request(rpc("/mcp", envelope("initialize", init_params("2099-01-01"), 1)));
    CHECK_STATUS(odd, 200);
    CHECK_EQ(odd.json()["result"]["protocolVersion"].get<std::string>(), "2025-06-18");

    Message tools = server.request(rpc("/mcp", tools_list(), initialize(server)));
    CHECK_STATUS(tools, 200);
    CHECK(tools.json()["result"]["tools"].size() >= 4);
}

E2E_CASE(initialize_on_a_live_session) {
    Server server(free_port(), {"--quiet"});
    const std::string session = initialize(server);

    Message again =
        server.request(rpc("/mcp", envelope("initialize", init_params(), 7), session));
    CHECK_STATUS(again, 400);
    CHECK_EQ(again.json()["error"].get<std::string>(), "unexpected_session_id");
    CHECK(again.json().contains("message"));
    CHECK(!again.json().contains("id"));
}

E2E_CASE(before_the_handshake) {
    Server server(free_port(), {"--quiet"});

    Message opened =
        server.request(rpc("/mcp", envelope("initialize", init_params(), 1)));
    CHECK_STATUS(opened, 200);
    const std::string session = opened.header("Mcp-Session-Id");
    CHECK(!session.empty());

    // Lenient is the default, so a session that has not finished the handshake is
    // still served; strict makes the client finish it first (see policy_strict).
    Message early = server.request(rpc("/mcp", tools_list(), session));
    CHECK_STATUS(early, 200);
    CHECK(early.json()["result"]["tools"].size() >= 4);


    // A session this server never issued.
    Message stranger = server.request(rpc("/mcp", tools_list(), std::string(40, 'f')));
    CHECK_STATUS(stranger, 404);
    CHECK_EQ(stranger.json()["error"].get<std::string>(), "session_not_found");

    // Lenient is the default: no session at all is still served.
    Message bare = server.request(rpc("/mcp", tools_list()));
    CHECK_STATUS(bare, 200);
    CHECK(bare.json()["result"]["tools"].size() >= 4);

    // The handshake completes, and the session keeps working.
    CHECK_STATUS(server.request(rpc("/mcp", envelope("notifications/initialized", Json(), -1), session)),
                 202);
    CHECK_STATUS(server.request(rpc("/mcp", tools_list(), session)), 200);
}

E2E_CASE(server_info) {
    Server plain(free_port(), {"--quiet"});
    Message plain_answer = plain.request(rpc("/mcp", envelope("initialize", init_params(), 1)));
    CHECK_STATUS(plain_answer, 200);
    const Json plain_info = plain_answer.json()["result"]["serverInfo"];
    CHECK_EQ(plain_info["name"].get<std::string>(), "ggmcp");
    CHECK_EQ(plain_info["title"].get<std::string>(), "ggmcp");
    CHECK(plain_info.contains("version"));
    CHECK(!plain_answer.json()["result"].contains("instructions"));

    Server named(free_port(), {"--quiet", "--name", "workshop", "--title", "The Workshop",
                               "--instructions", "Say please."});
    Message named_answer = named.request(rpc("/mcp", envelope("initialize", init_params(), 1)));
    CHECK_STATUS(named_answer, 200);
    const Json info = named_answer.json()["result"]["serverInfo"];
    CHECK_EQ(info["name"].get<std::string>(), "workshop");
    CHECK_EQ(info["title"].get<std::string>(), "The Workshop");
    CHECK_EQ(named_answer.json()["result"]["instructions"].get<std::string>(), "Say please.");

    // An empty title falls back to the name.
    Server bare(free_port(), {"--quiet", "--name", "forge"});
    CHECK_EQ(bare.request(rpc("/mcp", envelope("initialize", init_params(), 1)))
                 .json()["result"]["serverInfo"]["title"]
                 .get<std::string>(),
             "forge");
}

E2E_CASE(methods) {
    Server server(free_port(), {"--quiet"});
    const std::string session = initialize(server);

    Message ping = server.request(rpc("/mcp", envelope("ping", Json(), 3), session));
    CHECK_STATUS(ping, 200);
    CHECK_EQ(ping.json()["result"].dump(), std::string("{}"));

    Message resources =
        server.request(rpc("/mcp", envelope("resources/list", Json(), 4), session));
    CHECK_STATUS(resources, 200);
    CHECK_EQ(resources.json()["result"]["resources"].dump(), std::string("[]"));

    Message prompts = server.request(rpc("/mcp", envelope("prompts/list", Json(), 5), session));
    CHECK_STATUS(prompts, 200);
    CHECK_EQ(prompts.json()["result"]["prompts"].dump(), std::string("[]"));

    Message unknown =
        server.request(rpc("/mcp", envelope("tasks/forget", Json(), 6), session));
    CHECK_STATUS(unknown, 200);  // a JSON-RPC error still rides at 200
    CHECK_EQ(unknown.json()["error"]["code"].get<int>(), -32601);
    CHECK_EQ(unknown.json()["error"]["data"]["method"].get<std::string>(), "tasks/forget");
}

E2E_CASE(malformed_bodies) {
    Server server(free_port(), {"--quiet"});
    const std::string session = initialize(server);

    Message batched = server.request(
        rpc("/mcp", Json::parse("[{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"ping\"}]"), session));
    CHECK_STATUS(batched, 200);
    CHECK_EQ(batched.json()["error"]["code"].get<int>(), -32600);

    Call broken = post("/mcp", "{not json");
    broken.authority = server.authority();
    Message unparsable = server.request(broken);
    CHECK_STATUS(unparsable, 200);
    CHECK_EQ(unparsable.json()["error"]["code"].get<int>(), -32700);

    Call empty = post("/mcp", "");
    empty.authority = server.authority();
    Message blank = server.request(empty);
    CHECK_STATUS(blank, 200);
    CHECK_EQ(blank.json()["error"]["code"].get<int>(), -32700);

    Message no_method = server.request(rpc("/mcp", Json::parse("{\"jsonrpc\":\"2.0\",\"id\":9}"), session));
    CHECK_STATUS(no_method, 200);
    CHECK_EQ(no_method.json()["error"]["code"].get<int>(), -32600);
}

E2E_CASE(three_error_shapes) {
    Server server(free_port(), {"--quiet", "--session-policy", "strict"});

    // A JSON-RPC error: 200, `error` is an object, `id` comes back.
    Message refused = server.request(rpc("/mcp", envelope("no/such", Json(), 11)));
    CHECK_STATUS(refused, 400);  // strict refuses it before the method table
    Message inside = server.request(rpc("/mcp", Json::parse("[1]"), initialize(server)));
    CHECK_STATUS(inside, 200);
    CHECK(inside.json()["error"].is_object());
    CHECK(inside.json()["id"].is_null());  // a batch has no id to hand back

    // A protocol fault: a fault status, `error` is a string, no id.
    Message fault = server.request(rpc("/mcp", tools_list(), std::string(40, 'a')));
    CHECK_STATUS(fault, 404);
    CHECK(fault.json()["error"].is_string());
    CHECK(!fault.json().contains("id"));

    // A routing fault: a fault status, `error` is the status number.
    Message routing = server.request(get("/mcp"));
    CHECK_STATUS(routing, 405);
    CHECK(routing.json()["error"].is_number());
    CHECK_EQ(routing.json()["error"].get<int>(), 405);
    CHECK_EQ(routing.json()["message"].get<std::string>(), "method not allowed");
}

E2E_CASE(policy_strict) {
    Server server(free_port(), {"--quiet", "--session-policy", "strict"});

    Message bare = server.request(rpc("/mcp", tools_list()));
    CHECK_STATUS(bare, 400);
    CHECK_EQ(bare.json()["error"].get<std::string>(), "session_required");

    // Strict makes the client finish the handshake before it serves anything else.
    Message opened = server.request(rpc("/mcp", envelope("initialize", init_params(), 3)));
    CHECK_STATUS(opened, 200);
    CHECK(!opened.header("Mcp-Session-Id").empty());

    Message early = server.request(rpc("/mcp", tools_list(), opened.header("Mcp-Session-Id")));
    CHECK_STATUS(early, 400);
    CHECK_EQ(early.json()["error"].get<std::string>(), "session_not_initialized");

    CHECK_STATUS(server.request(rpc("/mcp", envelope("notifications/initialized", Json(), -1),
                                   opened.header("Mcp-Session-Id"))),
                 202);

    const std::string session = initialize(server);
    CHECK_STATUS(server.request(rpc("/mcp", tools_list(), session)), 200);

    Message gone = server.request(rpc("/mcp", tools_list(), std::string(40, 'b')));
    CHECK_STATUS(gone, 404);
    CHECK_EQ(gone.json()["error"].get<std::string>(), "session_not_found");
}

E2E_CASE(policy_stateless) {
    Server server(free_port(), {"--quiet", "--session-policy", "stateless"});

    Message opened = server.request(rpc("/mcp", envelope("initialize", init_params(), 1)));
    CHECK_STATUS(opened, 200);
    CHECK(opened.header("Mcp-Session-Id").empty());

    // Nothing is kept, so nothing is refused for carrying a stranger's id.
    CHECK_STATUS(server.request(rpc("/mcp", tools_list(), std::string(40, 'c'))), 200);
    CHECK_STATUS(server.request(rpc("/mcp", tools_list())), 200);

    Message closed = server.request(del("/mcp").with("Mcp-Session-Id", std::string(40, 'c')));
    CHECK_STATUS(closed, 404);
    CHECK_EQ(closed.json()["error"].get<std::string>(), "session_not_found");
}

E2E_CASE(session_teardown) {
    Server server(free_port(), {"--quiet"});
    const std::string session = initialize(server);

    Message tools = server.request(rpc("/mcp", tools_list(), session));
    CHECK_STATUS(tools, 200);
    CHECK_EQ(server.request(get("/health")).json()["sessions"].get<int>(), 1);

    Message closed = server.request(del("/mcp").with("Mcp-Session-Id", session));
    CHECK_STATUS(closed, 200);
    CHECK_EQ(closed.json()["closed"].get<bool>(), true);
    CHECK_EQ(closed.json()["session"].get<std::string>(), session);

    Message after = server.request(rpc("/mcp", tools_list(), session));
    CHECK_STATUS(after, 404);
    CHECK_EQ(after.json()["error"].get<std::string>(), "session_not_found");

    CHECK_EQ(server.request(get("/health")).json()["sessions"].get<int>(), 0);

    Message stranger = server.request(del("/mcp").with("Mcp-Session-Id", std::string(40, 'd')));
    CHECK_STATUS(stranger, 404);

    Message nothing = server.request(del("/mcp"));
    CHECK_STATUS(nothing, 400);
    CHECK_EQ(nothing.json()["error"].get<std::string>(), "bad_request");
}

E2E_CASE(protocol_version_header) {
    Server server(free_port(), {"--quiet"});
    const std::string session = initialize(server);

    CHECK_STATUS(server.request(rpc("/mcp", tools_list(), session).with("MCP-Protocol-Version", "2025-06-18")),
                 200);
    CHECK_STATUS(server.request(rpc("/mcp", tools_list(), session).with("MCP-Protocol-Version", "2024-11-05")),
                 200);

    Message refused =
        server.request(rpc("/mcp", tools_list(), session).with("MCP-Protocol-Version", "2050-01-01"));
    CHECK_STATUS(refused, 400);
    CHECK_EQ(refused.json()["error"].get<std::string>(), "unsupported_protocol_version");

    // The header is optional.
    CHECK_STATUS(server.request(rpc("/mcp", tools_list(), session)), 200);
}

E2E_CASE(notifications_answer_202) {
    Server server(free_port(), {"--quiet"});
    const std::string session = initialize(server);

    Message known =
        server.request(rpc("/mcp", envelope("notifications/cancelled", Json(), -1), session));
    CHECK_STATUS(known, 202);
    CHECK_EQ(known.body, std::string(""));
    CHECK(!known.has("Content-Type") || known.header("Content-Type").find("json") != std::string::npos);

    Message cancelled = server.request(
        rpc("/mcp", Json::parse("{\"jsonrpc\":\"2.0\",\"method\":\"notifications/initialized\"}"),
            session));
    CHECK_STATUS(cancelled, 202);
    CHECK_EQ(cancelled.body, std::string(""));
}
