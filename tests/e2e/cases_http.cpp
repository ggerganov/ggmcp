#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>
#include <stdlib.h>

#include <chrono>
#include <string>
#include <thread>

#include "mcp.hpp"

using namespace e2e;

E2E_CASE(health) {
    TempDir root;
    Server server(free_port(), {"--quiet", "--fs-root", root.path(), "--session-policy", "strict",
                                "--alias", "shop"});

    Message answer = server.request(get("/health"));
    CHECK_STATUS(answer, 200);
    const Json body = answer.json();
    CHECK_EQ(body["status"].get<std::string>(), "ok");
    CHECK_EQ(body["policy"].get<std::string>(), "strict");
    CHECK_EQ(body["alias"].get<std::string>(), "shop");
    CHECK_EQ(body["root"].get<std::string>(), root.path());
    CHECK_EQ(body["confined"].get<bool>(), true);
    CHECK_EQ(body["endpoint"].get<std::string>(), "/mcp");
    CHECK_EQ(body["transport"].get<std::string>(), "http");
    CHECK_EQ(body["url"].get<std::string>(), server.url("/mcp"));
    CHECK_EQ(body["sessions"].get<int>(), 0);
    CHECK_EQ(body["auth"].get<std::string>(), "off");

    const Json tools = body["tools"];
    CHECK(tools.is_array());
    CHECK_EQ(tools[0].get<std::string>(), "shop-hello");
    CHECK_HAS(tools.dump(), "shop-utc_now");
    CHECK_HAS(tools.dump(), "shop-bash");

    initialize(server);
    CHECK_EQ(server.request(get("/health")).json()["sessions"].get<int>(), 1);
}

E2E_CASE(index_page) {
    Server server(free_port(), {"--quiet"});
    Message answer = server.request(get("/"));
    CHECK_STATUS(answer, 200);
    CHECK_HAS(answer.header("Content-Type"), "text/html");
    CHECK_HAS(answer.body, "/mcp");
    CHECK_HAS(answer.body, "mcp-hello");
    CHECK_HAS(answer.body, "/health");
}

E2E_CASE(custom_endpoint) {
    Server server(free_port(), {"--quiet", "--endpoint", "/rpc"});

    const std::string session = initialize(server, "/rpc");
    CHECK(!session.empty());
    CHECK_STATUS(server.request(rpc("/rpc", envelope("ping", Json(), 2), session)), 200);

    // What used to be the endpoint is just another path now.
    CHECK_STATUS(server.request(get("/mcp")), 404);
    CHECK_STATUS(server.request(get("/rpc")), 405);

    Message index = server.request(get("/"));
    CHECK_STATUS(index, 200);
    CHECK_HAS(index.body, "/rpc");
    CHECK(index.body.find("/mcp") == std::string::npos);
}

E2E_CASE(transport_status_codes) {
    Server server(free_port(), {"--quiet", "--max-body", "4096"});
    const Json ping = envelope("ping", Json(), 3);
    const std::string session = initialize(server);

    // 405: a verb this server does not register, on a path it does know.
    Message not_a_verb = server.request(get("/mcp"));
    CHECK_STATUS(not_a_verb, 405);
    CHECK_HAS(not_a_verb.header("Allow"), "POST");
    CHECK_STATUS(server.request(verb("PUT", "/mcp")), 405);
    CHECK_STATUS(server.request(verb("TRACE", "/mcp")), 405);
    CHECK_STATUS(server.request(verb("PATCH", "/mcp")), 405);

    // 406: an Accept that rules JSON out.
    Call html = rpc("/mcp", ping, session);
    html.accept = "text/html";
    Message refused = server.request(html);
    CHECK_STATUS(refused, 406);
    CHECK_EQ(refused.json()["error"].get<std::string>(), "not_acceptable");

    Call star = rpc("/mcp", ping, session);
    star.accept = "*/*";
    CHECK_STATUS(server.request(star), 200);

    Call quiet = rpc("/mcp", ping, session);
    quiet.accept = "";
    CHECK_STATUS(server.request(quiet), 200);

    Call both = rpc("/mcp", ping, session);
    both.accept = "text/html, application/json;q=0.9";
    CHECK_STATUS(server.request(both), 200);

    // 415: a body that is not JSON.
    Call form = rpc("/mcp", ping, session);
    form.content_type = "application/x-www-form-urlencoded";
    Message type = server.request(form);
    CHECK_STATUS(type, 415);
    CHECK_EQ(type.json()["error"].get<std::string>(), "unsupported_media_type");

    Call bare = rpc("/mcp", ping, session);
    bare.send_content_type = false;
    CHECK_STATUS(server.request(bare), 415);

    Call utf8 = rpc("/mcp", ping, session);
    utf8.content_type = "application/json; charset=utf-8";
    CHECK_STATUS(server.request(utf8), 200);

    // 413: a body over the limit, with an empty body on the way out.
    Json large = envelope("tools/call", tool_call("mcp-write", Json::parse(
                                     "{\"path\":\"x\",\"content\":\"" + std::string(5000, 'x') + "\"}")),
                          4);
    Call oversize = rpc("/mcp", large, session);
    oversize.body = large.dump();
    Message too_big = server.request(oversize);
    CHECK_STATUS(too_big, 413);
    CHECK_EQ(too_big.body, std::string(""));

    // 404: a path this server does not have, as a routing fault.
    Message missing = server.request(get("/nowhere"));
    CHECK_STATUS(missing, 404);
    CHECK(missing.json()["error"].is_number());

    CHECK_STATUS(server.request(get("/health")), 200);
}

E2E_CASE(keep_alive_and_chunked_bodies) {
    Server server(free_port(), {"--quiet"});
    const Json ping = envelope("ping", Json(), 3);

    {
        Connection connection = server.connect();
        Call first = rpc("/mcp", envelope("initialize", init_params(), 1));
        first.connection = "keep-alive";
        Message opened = connection.request(first);
        CHECK_STATUS(opened, 200);
        const std::string session = opened.header("Mcp-Session-Id");
        CHECK(!session.empty());

        connection.request(rpc("/mcp", envelope("notifications/initialized", Json(), -1), session));

        Call second = rpc("/mcp", ping, session);
        second.connection = "keep-alive";
        Message second_answer = connection.request(second);
        CHECK_STATUS(second_answer, 200);
        CHECK(second_answer.json()["result"].is_object());
        CHECK(second_answer.has("Keep-Alive") || second_answer.header("Connection") == "keep-alive");
        CHECK(connection.alive());

        // A third request, still on the same socket.
        CHECK_STATUS(connection.request(rpc("/mcp", envelope("ping", Json(), 4), session)), 200);
        CHECK(connection.alive());
    }

    // A third request, then a fresh connection: the listener keeps taking calls.
    {
        Connection connection = server.connect();
        for (int round = 0; round < 3; ++round) {
            Call health = get("/health");
            health.connection = "keep-alive";
            CHECK_STATUS(connection.request(health), 200);
            CHECK(connection.alive());
        }
        connection.close();
        CHECK(!connection.alive());
        CHECK_STATUS(server.request(get("/health")), 200);
    }

    // A chunked body, reassembled by the server before it is parsed.
    {
        Call chunked = post("/mcp", "");
        chunked.chunk_body = ping.dump();
        chunked.authority = server.authority();
        Message answer = server.request(chunked);
        CHECK_STATUS(answer, 200);
        CHECK_EQ(answer.json()["id"].get<int>(), 3);
    }

    // Several chunks, so the body arrives in pieces.
    {
        const std::string body = ping.dump();
        Call chunked = post("/mcp", "");
        chunked.body.clear();
        chunked.chunks.push_back(body.substr(0, 7));
        chunked.chunks.push_back(body.substr(7, 11));
        chunked.chunks.push_back(body.substr(18));
        chunked.authority = server.authority();
        Message answer = server.request(chunked);
        CHECK_STATUS(answer, 200);
        CHECK_EQ(answer.json()["id"].get<int>(), 3);
        CHECK_EQ(answer.json()["result"].dump(), std::string("{}"));
    }
}

E2E_CASE(start_up_banner) {
    TempDir root;
    ::setenv("E2E_BANNER_TOKEN", "a-token-long-enough-to-serve", 1);
    Server server(free_port(),
                  {"--fs-root", root.path(), "--alias", "shop", "--session-policy", "strict",
                   "--require-token", "--token-env", "E2E_BANNER_TOKEN"});

    const std::string banner = server.log_within(5000);
    CHECK_HAS(banner, "/mcp");                       // the endpoint
    CHECK_HAS(banner, "nlohmann/json");              // the JSON backend
    CHECK_HAS(banner, "cpp-httplib");                // the HTTP backend
    CHECK_HAS(banner, "strict");                     // the session policy
    CHECK_HAS(banner, root.path());                  // the root
    CHECK_HAS(banner, "E2E_BANNER_TOKEN");           // the authorization source
    CHECK_HAS(banner, "shop");                       // the alias
    CHECK_HAS(banner, "shop-hello");                 // the tools
    CHECK_HAS(banner, "shop-whoami");

    Server hushed(free_port(), {"--quiet", "--fs-root", root.path()});
    CHECK_STATUS(hushed.request(get("/health")), 200);
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    CHECK_EQ(hushed.log(), std::string(""));
}

E2E_CASE(protected_resource_metadata) {
    ::setenv("E2E_METADATA_TOKEN", "a-token-with-some-length", 1);
    Server server(free_port(), {"--quiet", "--require-token", "--token-env", "E2E_METADATA_TOKEN",
                                "--title", "Vault"});

    Message answer = server.request(get("/.well-known/oauth-protected-resource"));
    CHECK_STATUS(answer, 200);
    const Json body = answer.json();
    CHECK_EQ(body["resource"].get<std::string>(), server.url("/mcp"));
    CHECK_EQ(body["resource_name"].get<std::string>(), "Vault");
    CHECK(body["bearer_methods_supported"].is_array());
    CHECK_EQ(body["bearer_methods_supported"][0]["prefix"].get<std::string>(), "Bearer");

    // Everything else is behind the token.
    CHECK_STATUS(server.request(get("/health")), 401);
    CHECK_STATUS(server.request(get("/")), 401);
}

E2E_CASE(one_instance_per_port) {
    const int port = free_port();
    Server first(port, {"--quiet"});
    CHECK_STATUS(first.request(get("/health")), 200);

    Server second(port, {"--quiet"}, false, false);
    int code = -1;
    CHECK(second.exited(&code, 8000));
    CHECK_EQ(std::to_string(code), "2");
    CHECK_HAS(second.log(), "cannot bind");

    // The instance that got there first is untouched.
    CHECK_STATUS(first.request(get("/health")), 200);
}

E2E_CASE(a_hung_up_client_does_not_kill_the_server) {
    Server server(free_port(), {"--quiet"});
    const std::string session = initialize(server);

    // Hang up mid-request: the server must survive the broken pipe.
    for (int attempt = 0; attempt < 4; ++attempt) {
        Connection connection = server.connect();
        Call call = rpc("/mcp", envelope("ping", Json(), 1), session);
        call.body = std::string(4000000, 'x');  // far more than the server will accept
        call.send_content_length = true;
        connection.send(call);
        connection.close();  // walk away while it is still being written
    }

    CHECK_STATUS(server.request(get("/health")), 200);
    CHECK_STATUS(server.request(rpc("/mcp", envelope("ping", Json(), 2), session)), 200);
}
