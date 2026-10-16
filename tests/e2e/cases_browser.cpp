#include <sys/stat.h>
#include <unistd.h>

#include <cstdlib>
#include <string>
#include <thread>

#include "mcp.hpp"

using namespace e2e;

namespace {

Json ping() { return envelope("ping", Json(), 3); }

std::string open_session(Server& server, const std::string& token,
                         const std::string& endpoint = "/mcp") {
    Call first = rpc(endpoint, envelope("initialize", init_params(), 1));
    if (!token.empty()) first.with("Authorization", "Bearer " + token);
    first.authority = server.authority();
    Message opened = server.request(first);
    if (opened.status != 200) {
        throw Failure("initialize answered " + std::to_string(opened.status) + ": " + opened.body);
    }
    const std::string session = opened.header("Mcp-Session-Id");
    Call done = rpc(endpoint, envelope("notifications/initialized", Json(), -1), session);
    if (!token.empty()) done.with("Authorization", "Bearer " + token);
    done.authority = server.authority();
    server.request(done);
    return session;
}

Call with_origin(Call call, const std::string& origin) {
    call.with("Origin", origin);
    return call;
}

Call with_token(Call call, const std::string& token) {
    call.with("Authorization", "Bearer " + token);
    return call;
}

// The token file is read again when it changes; give the clock a chance to move.
Message eventually_authorized(Server& server, const std::string& token, int attempts) {
    for (int attempt = 0; attempt < attempts; ++attempt) {
        Message answer = server.request(with_token(get("/health"), token));
        if (answer.status == 200) return answer;
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    return server.request(with_token(get("/health"), token));
}

}  // namespace

E2E_CASE(any_origin_is_allowed_by_default) {
    Server server(free_port(), {"--quiet"});
    const std::string session = initialize(server);

    Message answer = server.request(with_origin(rpc("/mcp", ping(), session), "http://agent.example"));
    CHECK_STATUS(answer, 200);
    CHECK_EQ(answer.header("Access-Control-Allow-Origin"), "http://agent.example");
    CHECK_EQ(answer.header("Vary"), "Origin");
    CHECK_HAS(answer.header("Access-Control-Expose-Headers"), "Mcp-Session-Id");
    CHECK_HAS(answer.header("Access-Control-Expose-Headers"), "MCP-Protocol-Version");

    // The origin is echoed rather than widened to `*`.
    CHECK(answer.header("Access-Control-Allow-Origin") != "*");

    // A request with no Origin needs no CORS headers.
    Message plain = server.request(rpc("/mcp", ping(), session));
    CHECK_STATUS(plain, 200);
    CHECK(!plain.has("Access-Control-Allow-Origin"));

    // The session id an initialize issues is readable, which is the point of exposing it.
    Message opened = server.request(with_origin(rpc("/mcp", envelope("initialize", init_params(), 1)),
                                                "http://agent.example"));
    CHECK_STATUS(opened, 200);
    CHECK(!opened.header("Mcp-Session-Id").empty());
    CHECK_HAS(opened.header("Access-Control-Expose-Headers"), "Mcp-Session-Id");
}

E2E_CASE(strict_origin) {
    Server strict(free_port(), {"--quiet", "--strict-origin"});
    const std::string session = initialize(strict);

    // The Host origin is always allowed.
    Message own = strict.request(
        with_origin(rpc("/mcp", ping(), session), "http://" + strict.authority()));
    CHECK_STATUS(own, 200);
    CHECK_EQ(own.header("Access-Control-Allow-Origin"), "http://" + strict.authority());

    // Anything else is a 403 that says nothing to the browser.
    Message refused = strict.request(with_origin(rpc("/mcp", ping(), session), "http://evil.example"));
    CHECK_STATUS(refused, 403);
    CHECK(!refused.has("Access-Control-Allow-Origin"));
    CHECK(!refused.has("Vary"));
    CHECK(refused.json()["error"].is_number());
    CHECK_EQ(refused.json()["error"].get<int>(), 403);

    // A named origin joins the Host origin.
    Server named(free_port(),
                 {"--quiet", "--strict-origin", "--allow-origin", "http://localhost:3000",
                  "--allow-origin", "https://elsewhere.example"});
    const std::string named_session = initialize(named);
    Message named_origin =
        named.request(with_origin(rpc("/mcp", ping(), named_session), "http://localhost:3000"));
    CHECK_STATUS(named_origin, 200);
    CHECK_EQ(named_origin.header("Access-Control-Allow-Origin"), "http://localhost:3000");
    CHECK_STATUS(named.request(with_origin(rpc("/mcp", ping(), named_session), "http://localhost:3001")),
                 403);

    // `*` stands for any origin, named explicitly.
    Server star(free_port(), {"--quiet", "--strict-origin", "--allow-origin", "*"});
    const std::string star_session = initialize(star);
    CHECK_STATUS(star.request(with_origin(rpc("/mcp", ping(), star_session), "http://anywhere.example")),
                 200);

    // Even the index and the health page refuse a stranger.
    CHECK_STATUS(strict.request(with_origin(get("/health"), "http://evil.example")), 403);
    CHECK_STATUS(strict.request(with_origin(get("/"), "http://evil.example")), 403);
    CHECK_STATUS(strict.request(get("/health")), 200);
}

E2E_CASE(cors_preflight) {
    Server server(free_port(), {"--quiet"});

    Call plain = verb("OPTIONS", "/mcp");
    plain.with("Origin", "http://agent.example");
    plain.with("Access-Control-Request-Method", "POST");
    plain.authority = server.authority();
    Message answer = server.request(plain);
    CHECK_STATUS(answer, 204);
    CHECK_EQ(answer.header("Access-Control-Allow-Origin"), "http://agent.example");
    CHECK_HAS(answer.header("Access-Control-Allow-Methods"), "POST");
    CHECK_HAS(answer.header("Access-Control-Allow-Methods"), "DELETE");
    CHECK_HAS(answer.header("Access-Control-Allow-Headers"), "Mcp-Session-Id");
    CHECK_HAS(answer.header("Access-Control-Allow-Headers"), "Authorization");
    CHECK(!answer.header("Access-Control-Max-Age").empty());
    CHECK_EQ(answer.header("Vary"), "Origin");

    // The headers the client asked about come back.
    Call asked = verb("OPTIONS", "/mcp");
    asked.with("Access-Control-Request-Headers", "X-Agent-Trace, Mcp-Session-Id");
    asked.with("Access-Control-Request-Method", "POST");
    asked.with("Origin", "http://agent.example");
    asked.authority = server.authority();
    CHECK_HAS(server.request(asked).header("Access-Control-Allow-Headers"), "X-Agent-Trace");

    // A verb this server does not register is refused, before the browser asks twice.
    Call wrong = verb("OPTIONS", "/mcp");
    wrong.with("Access-Control-Request-Method", "PUT");
    wrong.with("Origin", "http://agent.example");
    wrong.authority = server.authority();
    CHECK_STATUS(server.request(wrong), 405);

    // The preflight is never authorized: it carries no token by definition.
    ::setenv("E2E_PREFLIGHT_TOKEN", "a-token-of-some-length", 1);
    Server guarded(free_port(), {"--quiet", "--require-token", "--token-env", "E2E_PREFLIGHT_TOKEN"});
    Call guarded_preflight = verb("OPTIONS", "/mcp");
    guarded_preflight.with("Access-Control-Request-Method", "POST");
    guarded_preflight.with("Origin", "http://agent.example");
    guarded_preflight.authority = guarded.authority();
    CHECK_STATUS(guarded.request(guarded_preflight), 204);
}

E2E_CASE(no_cors) {
    Server server(free_port(), {"--quiet", "--no-cors"});
    const std::string session = initialize(server);

    Message answer = server.request(with_origin(rpc("/mcp", ping(), session), "http://agent.example"));
    CHECK_STATUS(answer, 200);
    CHECK(!answer.has("Access-Control-Allow-Origin"));
    CHECK(!answer.has("Access-Control-Expose-Headers"));

    Call preflight = verb("OPTIONS", "/mcp");
    preflight.with("Access-Control-Request-Method", "POST");
    preflight.with("Origin", "http://agent.example");
    preflight.authority = server.authority();
    CHECK_STATUS(server.request(preflight), 405);
}

E2E_CASE(a_refused_origin_carries_nothing) {
    Server strict(free_port(), {"--quiet", "--strict-origin"});
    Message refused = strict.request(with_origin(get("/health"), "http://evil.example"));
    CHECK_STATUS(refused, 403);
    for (const Header& header : refused.headers) {
        CHECK(header.name.find("Access-Control") == std::string::npos);
    }
}

E2E_CASE(bearer_token) {
    const std::string token = "correct-horse-battery-staple";
    ::setenv("E2E_TOKEN", token.c_str(), 1);
    Server server(free_port(), {"--quiet", "--require-token", "--token-env", "E2E_TOKEN"});

    // Nothing at all is behind the token until it is given.
    Message none = server.request(get("/health"));
    CHECK_STATUS(none, 401);
    CHECK_HAS(none.header("WWW-Authenticate"), "Bearer");
    CHECK_HAS(none.header("WWW-Authenticate"), "/.well-known/oauth-protected-resource");
    const Json challenge_body = none.json();
    CHECK_EQ(challenge_body.at("resource").get<std::string>(), server.url("/mcp"));
    CHECK(!challenge_body.contains("error"));

    Message wrong = server.request(with_token(get("/health"), "not-the-token"));
    CHECK_STATUS(wrong, 401);
    CHECK_STATUS(server.request(with_token(get("/"), "not-the-token")), 401);

    // Another scheme, or no scheme at all, is not this token.
    CHECK_STATUS(server.request(get("/health").with("Authorization", "Basic c2VjcmV0OnNlY3JldA==")),
                 401);

    // The right token, in the header, works.
    CHECK_STATUS(server.request(with_token(get("/health"), token)), 200);
    const std::string session = open_session(server, token);
    CHECK(!session.empty());
    CHECK_STATUS(server.request(with_origin(with_token(rpc("/mcp", ping(), session), token),
                                           "http://agent.example")),
                 200);

    // A 401 carries the CORS headers too, so a browser can read the challenge.
    Message challenge = server.request(
        with_origin(get("/health"), "http://agent.example"));
    CHECK_STATUS(challenge, 401);
    CHECK_EQ(challenge.header("Access-Control-Allow-Origin"), "http://agent.example");
    CHECK(challenge.has("WWW-Authenticate"));

    // The endpoint refuses too, before the handshake.
    CHECK_STATUS(server.request(rpc("/mcp", envelope("initialize", init_params(), 1))), 401);

    // health says the door is guarded.
    CHECK_EQ(server.request(with_token(get("/health"), token)).json()["auth"].get<std::string>(),
             "token");
}

E2E_CASE(a_token_in_the_query_string_is_refused) {
    const std::string token = "a-query-string-token";
    ::setenv("E2E_QUERY_TOKEN", token.c_str(), 1);
    Server server(free_port(), {"--quiet", "--require-token", "--token-env", "E2E_QUERY_TOKEN"});

    // The query is the problem, not the token: even the right token there is a 401.
    Call query = get("/health");
    query.path = "/health?token=" + token;
    query.with("Authorization", "Bearer " + token);
    query.authority = server.authority();
    Message answer = server.request(query);
    CHECK_STATUS(answer, 401);
    CHECK(answer.has("WWW-Authenticate"));

    Call encoded = get("/health");
    encoded.path = "/health?access_token=" + token;
    encoded.with("Authorization", "Bearer " + token);
    encoded.authority = server.authority();
    CHECK_STATUS(server.request(encoded), 401);

    // The same token in the header is fine.
    CHECK_STATUS(server.request(with_token(get("/health"), token)), 200);

    // With authorization off, a query token means nothing.
    Server open(free_port(), {"--quiet"});
    Call harmless = get("/health");
    harmless.path = "/health?token=whatever";
    harmless.authority = open.authority();
    CHECK_STATUS(open.request(harmless), 200);
}

E2E_CASE(token_file_rotation) {
    TempDir dir;
    dir.write("token", "first-token-value\n");
    Server server(free_port(), {"--quiet", "--require-token", "--token-file", dir.file("token")});

    CHECK_STATUS(server.request(with_token(get("/health"), "first-token-value")), 200);
    CHECK_STATUS(server.request(with_token(get("/health"), "second-token-value")), 401);

    // Rotate the file, and the new token works without a restart.
    dir.write("token", "second-token-value\n");
    CHECK_STATUS(eventually_authorized(server, "second-token-value", 60), 200);
    CHECK_STATUS(server.request(with_token(get("/health"), "first-token-value")), 401);

    // The endpoint follows the same rule.
    CHECK(!open_session(server, "second-token-value").empty());
}

E2E_CASE(an_unusable_token_source_is_fatal) {
    ::unsetenv("E2E_NOBODY_SETS_THIS");

    Server missing_env(free_port(), {"--quiet", "--require-token", "--token-env", "E2E_NOBODY_SETS_THIS"},
                       false, false);
    int code = -1;
    CHECK(missing_env.exited(&code, 8000));
    CHECK_EQ(std::to_string(code), "2");
    CHECK_HAS(missing_env.log(), "E2E_NOBODY_SETS_THIS");
    CHECK_HAS(missing_env.log(), "ggmcp:");

    Server missing_file(free_port(),
                        {"--quiet", "--require-token", "--token-file", "/no/such/token/file"}, false,
                        false);
    int file_code = -1;
    CHECK(missing_file.exited(&file_code, 8000));
    CHECK_EQ(std::to_string(file_code), "2");
    CHECK_HAS(missing_file.log(), "token");

    // A token on the command line is not offered at all.
    Server flag(free_port(), {"--quiet", "--token", "not-a-real-flag"}, false, false);
    int flag_code = -1;
    CHECK(flag.exited(&flag_code, 8000));
    CHECK_EQ(std::to_string(flag_code), "2");
    CHECK_HAS(flag.log(), "unknown flag");

    // Two sources at once is a mistake, not a tie break.
    Server both(free_port(),
                {"--quiet", "--require-token", "--token-file", "/no/such/token/file", "--token-env",
                 "E2E_NOBODY_SETS_THIS"},
                false, false);
    int both_code = -1;
    CHECK(both.exited(&both_code, 8000));
    CHECK_EQ(std::to_string(both_code), "2");
    CHECK_HAS(both.log(), "token source");
}

E2E_CASE(the_default_token_source_is_the_environment) {
    ::setenv("GGMCP_TOKEN", "the-default-token", 1);
    Server server(free_port(), {"--require-token"});

    CHECK_HAS(server.log(), "GGMCP_TOKEN");
    CHECK_STATUS(server.request(get("/health")), 401);
    CHECK_STATUS(server.request(with_token(get("/health"), "the-default-token")), 200);
}
