#include <sys/stat.h>
#include <unistd.h>

#include <chrono>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include "mcp.hpp"

using namespace e2e;

namespace {

Json call(Server& server, const std::string& session, const std::string& tool,
          const Json& arguments, long long id = 5) {
    Message answer = server.request(rpc("/mcp", envelope("tools/call", tool_call(tool, arguments), id),
                                       session));
    return answer.json();
}

Json shell(const std::string& command, long long timeout = 0) {
    Json arguments;
    arguments["command"] = command;
    if (timeout > 0) arguments["timeout"] = timeout;
    return arguments;
}

Json read_of(const std::string& path, long long offset = 0, long long limit = 0) {
    Json arguments;
    arguments["path"] = path;
    if (offset > 0) arguments["offset"] = offset;
    if (limit > 0) arguments["limit"] = limit;
    return arguments;
}

Json write_of(const std::string& path, const std::string& content, bool append = false) {
    Json arguments;
    arguments["append"] = append;
    arguments["content"] = content;
    arguments["path"] = path;
    return arguments;
}

Json edit_of(const std::string& path, const Json& edits, bool dry_run = false) {
    Json arguments;
    arguments["dryRun"] = dry_run;
    arguments["edits"] = edits;
    arguments["path"] = path;
    return arguments;
}

Json edit_item(const std::string& old_text, const std::string& new_text, bool replace = false) {
    Json item;
    item["newText"] = new_text;
    item["oldText"] = old_text;
    if (replace) item["replace"] = true;
    return item;
}

std::string joined(const std::vector<std::string>& items) {
    std::string out;
    for (std::size_t i = 0; i < items.size(); ++i) {
        if (i != 0) out += " ";
        out += items[i];
    }
    return out;
}

}  // namespace

E2E_CASE(tools_list) {
    TempDir root;
    Server server(free_port(), {"--quiet", "--fs-root", root.path()});
    const std::string session = initialize(server);

    Message list = server.request(rpc("/mcp", envelope("tools/list", Json(), 2), session));
    CHECK_STATUS(list, 200);
    const Json tools = list.json()["result"]["tools"];
    CHECK_EQ(std::to_string(tools.size()), "8");
    for (const Json& tool : tools) {
        CHECK(tool["name"].is_string());
        CHECK(tool["title"].is_string());
        CHECK(tool["description"].is_string());
        CHECK(tool["inputSchema"].is_object());
        CHECK_EQ(tool["inputSchema"]["type"].get<std::string>(), "object");
        CHECK(tool["inputSchema"]["properties"].is_object());
        // An empty `required` is not valid JSON Schema.
        if (tool["inputSchema"].value("required", Json::array()).empty()) {
            CHECK(!tool["inputSchema"].contains("required"));
        } else {
            CHECK(tool["inputSchema"]["required"].is_array());
            CHECK(!tool["inputSchema"]["required"].empty());
            for (const Json& key : tool["inputSchema"]["required"]) {
                CHECK(tool["inputSchema"]["properties"].contains(key.get<std::string>()));
            }
        }
    }

    Json by_name;
    for (const Json& tool : tools) by_name[tool["name"].get<std::string>()] = tool;
    for (const std::string& name : {"mcp-hello", "mcp-echo", "mcp-utc_now", "mcp-whoami", "mcp-read",
                                    "mcp-write", "mcp-edit", "mcp-bash"}) {
        CHECK(by_name.contains(name));
    }
    CHECK_EQ(by_name["mcp-read"]["inputSchema"]["required"][0].get<std::string>(), "path");
    CHECK_EQ(by_name["mcp-echo"]["inputSchema"]["required"][0].get<std::string>(), "message");
}

E2E_CASE(always_on_tools) {
    TempDir root;
    Server server(free_port(), {"--quiet", "--fs-root", root.path()});
    const std::string session = initialize(server);

    Json greet;
    greet["name"] = "Ada";
    const Json hello = call(server, session, "mcp-hello", greet);
    CHECK(!is_error(hello));
    CHECK_EQ(text_of(hello), "Hello, Ada!");

    const Json plain = call(server, session, "mcp-hello", Json::object());
    CHECK_EQ(text_of(plain), "Hello, world!");

    Json loud;
    loud["loud"] = true;
    loud["name"] = "ada";
    CHECK_EQ(text_of(call(server, session, "mcp-hello", loud)), "HELLO, ADA!");

    Json message;
    message["message"] = "the message, back unchanged";
    CHECK_EQ(text_of(call(server, session, "mcp-echo", message)), "the message, back unchanged");

    // echo with no message is a tool failure, not a protocol error.
    const Json silent = call(server, session, "mcp-echo", Json::object());
    CHECK(is_error(silent));
    CHECK_HAS(text_of(silent), "message");

    const Json now = Json::parse(text_of(call(server, session, "mcp-utc_now", Json::object())));
    CHECK(now["epochMillis"].get<long long>() > 1700000000000LL);
    const std::string iso = now["iso8601"].get<std::string>();
    CHECK_EQ(iso.size(), 24u);
    CHECK_EQ(iso.substr(4, 1), "-");
    CHECK_EQ(iso.substr(10, 1), "T");
    CHECK_EQ(iso.back(), 'Z');

    const Json who = Json::parse(text_of(call(server, session, "mcp-whoami", Json::object())));
    CHECK_EQ(who["transport"].get<std::string>(), "http");
    CHECK_EQ(who["session"].get<std::string>(), session);
    CHECK_EQ(who["root"].get<std::string>(), root.path());
    CHECK_EQ(who["confined"].get<bool>(), true);
    CHECK_HAS(who["address"].get<std::string>(), "127.0.0.1:");
}

E2E_CASE(tools_list_and_call_agree) {
    TempDir root;
    Server server(free_port(), {"--quiet", "--fs-root", root.path()});
    const std::string session = initialize(server);

    const Json listed =
        server.request(rpc("/mcp", envelope("tools/list", Json(), 2), session)).json();
    Json names = Json::array();
    for (const Json& tool : listed["result"]["tools"]) names.push_back(tool["name"]);

    // /health advertises exactly the names tools/list published.
    CHECK_EQ(server.request(get("/health")).json()["tools"].dump(), names.dump());

    // Every published name is callable; one that is not published is not.
    for (const Json& name : names) {
        Json params;
        params["arguments"] = Json::object();
        params["name"] = name;
        Message answer = server.request(rpc("/mcp", envelope("tools/call", params, 3), session));
        CHECK_STATUS(answer, 200);
        CHECK(answer.json().contains("result"));
    }
    Json unpublished;
    unpublished["arguments"] = Json::object();
    unpublished["name"] = "mcp-nothing";
    CHECK(call(server, session, "mcp-nothing", Json::object()).contains("error"));
}

E2E_CASE(a_failing_tool_is_not_a_protocol_error) {
    TempDir root;
    Server server(free_port(), {"--quiet", "--fs-root", root.path()});
    const std::string session = initialize(server);

    const Json missing = call(server, session, "mcp-read", read_of(root.file("nowhere.txt")));
    CHECK(is_error(missing));
    CHECK_HAS(text_of(missing), "error");

    // A wrong request, on the other hand, is a JSON-RPC error.
    Message no_name =
        server.request(rpc("/mcp", envelope("tools/call", Json::parse("{\"arguments\":{}}"), 6), session));
    CHECK_STATUS(no_name, 200);
    CHECK_EQ(no_name.json()["error"]["code"].get<int>(), -32602);
    CHECK(no_name.json()["error"]["data"]["available"].is_array());

    Json params;
    params["arguments"] = Json::object();
    params["name"] = "mcp-nonsense";
    Message unknown = server.request(rpc("/mcp", envelope("tools/call", params, 7), session));
    CHECK_STATUS(unknown, 200);
    CHECK_EQ(unknown.json()["error"]["code"].get<int>(), -32602);
    CHECK_HAS(unknown.json()["error"]["message"].get<std::string>(), "mcp-");
    CHECK_EQ(unknown.json()["error"]["data"]["tool"].get<std::string>(), "mcp-nonsense");
    CHECK_EQ(unknown.json()["error"]["data"]["prefix"].get<std::string>(), "mcp");

    Message arguments = server.request(
        rpc("/mcp",
            Json::parse("{\"jsonrpc\":\"2.0\",\"id\":8,\"method\":\"tools/call\",\"params\":"
                        "{\"name\":\"mcp-echo\",\"arguments\":\"not an object\"}}"),
            session));
    CHECK_STATUS(arguments, 200);
    CHECK_EQ(arguments.json()["error"]["code"].get<int>(), -32602);
}

E2E_CASE(tool_alias) {
    TempDir root;

    Server plain(free_port(), {"--quiet", "--fs-root", root.path()});
    const std::string plain_session = initialize(plain);
    CHECK_HAS(plain.request(rpc("/mcp", envelope("tools/list", Json(), 2), plain_session)).json().dump(),
              "mcp-hello");
    const Json bare = call(plain, plain_session, "hello", Json::object());
    CHECK(bare.contains("error"));
    CHECK_EQ(bare["error"]["data"]["prefix"].get<std::string>(), "mcp");

    Server studio(free_port(), {"--quiet", "--alias", "studio", "--fs-root", root.path()});
    const std::string studio_session = initialize(studio);
    const Json studio_list =
        studio.request(rpc("/mcp", envelope("tools/list", Json(), 2), studio_session)).json();
    for (const Json& tool : studio_list["result"]["tools"]) {
        CHECK_HAS(tool["name"].get<std::string>(), "studio-");
    }
    Json message;
    message["message"] = "named";
    CHECK_EQ(text_of(call(studio, studio_session, "studio-echo", message)), "named");
    CHECK(call(studio, studio_session, "mcp-echo", message).contains("error"));
    CHECK_EQ(studio.request(get("/health")).json()["alias"].get<std::string>(), "studio");

    Server bare_names(free_port(), {"--quiet", "--alias", "", "--fs-root", root.path()});
    const std::string bare_session = initialize(bare_names);
    const Json bare_list =
        bare_names.request(rpc("/mcp", envelope("tools/list", Json(), 2), bare_session)).json();
    for (const Json& tool : bare_list["result"]["tools"]) {
        CHECK(tool["name"].get<std::string>().find('-') == std::string::npos);
    }
    CHECK_EQ(text_of(call(bare_names, bare_session, "echo", message)), "named");
    CHECK(call(bare_names, bare_session, "mcp-echo", message).contains("error"));
    CHECK_EQ(bare_names.request(get("/health")).json()["alias"].get<std::string>(), "");
}

E2E_CASE(file_tools) {
    TempDir root;
    Server server(free_port(), {"--quiet", "--fs-root", root.path()});
    const std::string session = initialize(server);
    root.write("notes.txt", "one\ntwo\nthree\n");

    // write creates, and returns what it did.
    const Json written = call(server, session, "mcp-write", write_of("made.txt", "hello\n"));
    CHECK(!is_error(written));
    CHECK_EQ(root.read("made.txt"), "hello\n");
    CHECK_EQ(Json::parse(text_of(written))["bytes"].get<int>(), 6);

    // write replaces, atomically, and append adds.
    CHECK_EQ(root.read("notes.txt"), "one\ntwo\nthree\n");
    call(server, session, "mcp-write", write_of("notes.txt", "four\n"));
    CHECK_EQ(root.read("notes.txt"), "four\n");
    call(server, session, "mcp-write", write_of("notes.txt", "five\n", true));
    CHECK_EQ(root.read("notes.txt"), "four\nfive\n");

    // read returns the bytes it finds, by line when asked.
    root.write("lines.txt", "a\nb\nc\nd\n");
    CHECK_EQ(text_of(call(server, session, "mcp-read", read_of("lines.txt"))), "a\nb\nc\nd\n");
    CHECK_EQ(text_of(call(server, session, "mcp-read", read_of("lines.txt", 2, 2))), "b\nc");
    CHECK_EQ(text_of(call(server, session, "mcp-read", read_of("lines.txt", 3, 0))), "c\nd\n");
    CHECK_EQ(text_of(call(server, session, "mcp-read", read_of("lines.txt", 1, 1))), "a");
    CHECK_EQ(text_of(call(server, session, "mcp-read", read_of("lines.txt", 99, 5))), "");

    // a relative path is taken against the root, an absolute one inside the root as given
    CHECK_EQ(text_of(call(server, session, "mcp-read", read_of(root.file("lines.txt")))), "a\nb\nc\nd\n");
    CHECK_EQ(text_of(call(server, session, "mcp-read", read_of("./lines.txt"))), "a\nb\nc\nd\n");
    CHECK_EQ(text_of(call(server, session, "mcp-read", read_of("sub/../lines.txt"))), "a\nb\nc\nd\n");

    // read does not flinch at bytes that are not UTF-8
    {
        std::string bytes;
        bytes.push_back('\xff');
        bytes.push_back('\xfe');
        bytes.append("binary");
        bytes.push_back('\x00');
        root.write("bytes.bin", bytes);
        const Json binary = call(server, session, "mcp-read", read_of("bytes.bin"));
        CHECK(!is_error(binary));
        CHECK_HAS(text_of(binary), "binary");
    }

    // edit replaces exactly, all edits or none
    Json one_edit = Json::array();
    one_edit.push_back(edit_item("beta", "gamma"));
    Json every_edit = Json::array();
    every_edit.push_back(edit_item("beta", "gamma", true));
    Json two_edits = Json::array();
    two_edits.push_back(edit_item("alpha", "A"));
    two_edits.push_back(edit_item("nope", "B"));

    root.write("code.txt", "alpha beta alpha beta\n");
    const Json one = call(server, session, "mcp-edit", edit_of("code.txt", one_edit));
    CHECK(!is_error(one));
    CHECK_EQ(root.read("code.txt"), "alpha gamma alpha beta\n");

    root.write("code.txt", "alpha beta alpha beta\n");
    call(server, session, "mcp-edit", edit_of("code.txt", every_edit));
    CHECK_EQ(root.read("code.txt"), "alpha gamma alpha gamma\n");

    root.write("code.txt", "alpha beta\n");
    const Json both = call(server, session, "mcp-edit", edit_of("code.txt", two_edits));
    CHECK(is_error(both));
    CHECK_EQ(root.read("code.txt"), "alpha beta\n");  // the first edit did not stick either

    const Json dry = call(server, session, "mcp-edit", edit_of("code.txt", one_edit, true));
    CHECK(!is_error(dry));
    CHECK_EQ(Json::parse(text_of(dry))["dryRun"].get<bool>(), true);
    CHECK_EQ(root.read("code.txt"), "alpha beta\n");

    const Json empty_edits = call(server, session, "mcp-edit", edit_of("code.txt", Json::array()));
    CHECK(is_error(empty_edits));

    // a directory is not a file
    CHECK(is_error(call(server, session, "mcp-read", read_of(root.path()))));
}

E2E_CASE(failing_writes_leave_nothing_behind) {
    TempDir root;
    TempDir outside;
    Server server(free_port(), {"--quiet", "--fs-root", root.path()});
    const std::string session = initialize(server);

    root.write("target.txt", "keep me\n");
    outside.write("away.txt", "not yours\n");

    // A path under a file: nothing is made, and the file is untouched.
    CHECK(is_error(call(server, session, "mcp-write", write_of("target.txt/child.txt", "x"))));
    CHECK(!root.exists("target.txt/child.txt"));
    CHECK_EQ(root.read("target.txt"), "keep me\n");
    CHECK_EQ(joined(root.entries()), "target.txt");

    // A write outside the root makes nothing, out there or in here.
    CHECK(is_error(call(server, session, "mcp-write", write_of(outside.file("nope.txt"), "x"))));
    CHECK(!outside.exists("nope.txt"));
    CHECK_EQ(joined(root.entries()), "target.txt");

    // An edit outside the root changes nothing.
    Json away_edit = Json::array();
    away_edit.push_back(edit_item("not", "not"));
    CHECK(is_error(call(server, session, "mcp-edit",
                        edit_of(outside.file("away.txt"), away_edit))));
    CHECK_EQ(outside.read("away.txt"), "not yours\n");
    CHECK_EQ(joined(outside.entries()), "away.txt");

    // A read of a directory, and of a path with nothing behind it.
    CHECK(is_error(call(server, session, "mcp-write", write_of("", "x"))));
    CHECK_EQ(joined(root.entries()), "target.txt");

    // A write into a directory that does not exist yet does make it.
    CHECK(!is_error(call(server, session, "mcp-write", write_of("deep/down/x.txt", "made\n"))));
    CHECK_EQ(root.read("deep/down/x.txt"), "made\n");
}

E2E_CASE(confinement) {
    TempDir root;
    TempDir outside;
    Server server(free_port(), {"--quiet", "--fs-root", root.path()});
    const std::string session = initialize(server);

    outside.write("secret.txt", "hidden\n");
    root.write("near.txt", "yours\n");

    // A path outside the root is refused, and quotes the path that was asked for.
    const Json away = call(server, session, "mcp-read", read_of(outside.file("secret.txt")));
    CHECK(is_error(away));
    CHECK_HAS(text_of(away), outside.file("secret.txt"));
    CHECK_HAS(text_of(away), "outside the root");

    // Up out of the root and back down.
    const std::string root_name = root.path().substr(root.path().find_last_of('/') + 1);
    const std::string outside_name = outside.path().substr(outside.path().find_last_of('/') + 1);
    const std::string parent = root.path().substr(0, root.path().size() - root_name.size() - 1);
    CHECK(is_error(call(server, session, "mcp-read",
                        read_of(parent + "/" + root_name + "/../" + outside_name + "/secret.txt"))));

    // A symlink that leaves the root is refused, quoting the requested path.
    CHECK(::symlink(outside.file("secret.txt").c_str(), root.file("link.txt").c_str()) == 0);
    const Json through = call(server, session, "mcp-read", read_of("link.txt"));
    CHECK(is_error(through));
    CHECK_HAS(text_of(through), "link.txt");
    CHECK(text_of(through).find(outside.path()) == std::string::npos);

    // A symlink that stays inside the root is fine.
    CHECK(::symlink(root.file("near.txt").c_str(), root.file("here.txt").c_str()) == 0);
    CHECK_EQ(text_of(call(server, session, "mcp-read", read_of("here.txt"))), "yours\n");

    // A directory symlink out of the root, reached through a real path.
    CHECK(::symlink(outside.path().c_str(), root.file("out").c_str()) == 0);
    CHECK(is_error(call(server, session, "mcp-read", read_of("out/secret.txt"))));
    CHECK(is_error(call(server, session, "mcp-write", write_of("out/put.txt", "x"))));
    CHECK(!outside.exists("put.txt"));

    // A tilde is a name, not a home directory.
    CHECK(::mkdir(root.file("~").c_str(), 0700) == 0);
    root.write("~/note.txt", "tilde\n");
    root.write("note.txt", "plain\n");
    CHECK_EQ(text_of(call(server, session, "mcp-read", read_of("~/note.txt"))), "tilde\n");
    CHECK_EQ(text_of(call(server, session, "mcp-read", read_of("note.txt"))), "plain\n");
}

E2E_CASE(an_outside_root_is_taken_as_given) {
    TempDir root;
    TempDir outside;
    Server server(free_port(), {"--quiet", "--fs-root", root.path(), "--allow-outside-root"});
    const std::string session = initialize(server);
    outside.write("free.txt", "anywhere\n");

    const Json away = call(server, session, "mcp-read", read_of(outside.file("free.txt")));
    CHECK(!is_error(away));
    CHECK_EQ(text_of(away), "anywhere\n");

    CHECK(!is_error(call(server, session, "mcp-write", write_of(outside.file("more.txt"), "more\n"))));
    CHECK_EQ(outside.read("more.txt"), "more\n");

    // whoami says so, too.
    const Json who = Json::parse(text_of(call(server, session, "mcp-whoami", Json::object())));
    CHECK_EQ(who["confined"].get<bool>(), false);
    CHECK(who["root"].is_null());
    CHECK_EQ(server.request(get("/health")).json()["confined"].get<bool>(), false);
}

E2E_CASE(bash_tool) {
    TempDir root;
    TempDir outside;
    Server server(free_port(), {"--quiet", "--fs-root", root.path()});
    const std::string session = initialize(server);

    const Json hello = call(server, session, "mcp-bash", shell("echo hi; echo boom >&2"));
    CHECK(!is_error(hello));
    const Json ran = Json::parse(text_of(hello));
    CHECK_EQ(ran["stdout"].get<std::string>(), "hi\n");
    CHECK_EQ(ran["stderr"].get<std::string>(), "boom\n");
    CHECK_EQ(ran["exitCode"].get<int>(), 0);
    CHECK_EQ(ran["timedOut"].get<bool>(), false);
    CHECK_EQ(ran["command"].get<std::string>(), "echo hi; echo boom >&2");

    // A non-zero exit is a failed call, with the numbers still there to read.
    const Json failed = call(server, session, "mcp-bash", shell("exit 3"));
    CHECK(is_error(failed));
    CHECK_EQ(Json::parse(text_of(failed))["exitCode"].get<int>(), 3);

    // /bin/sh -c, with everything that means.
    CHECK_EQ(Json::parse(text_of(call(server, session, "mcp-bash", shell("printf abc | tr a-z A-Z"))))
                 ["stdout"]
                     .get<std::string>(),
             "ABC");
    const std::string marker = outside.file("from-shell.txt");
    CHECK(!is_error(call(server, session, "mcp-bash", shell("echo outside > " + marker))));
    CHECK_HAS(outside.read("from-shell.txt"), "outside");  // bash is not confined

    // The timeout stops a command that never ends.
    const auto began = std::chrono::steady_clock::now();
    const Json slow = call(server, session, "mcp-bash", shell("sleep 30; echo late", 1));
    const auto took = std::chrono::duration_cast<std::chrono::seconds>(
                          std::chrono::steady_clock::now() - began)
                          .count();
    CHECK(is_error(slow));
    CHECK_EQ(Json::parse(text_of(slow))["timedOut"].get<bool>(), true);
    CHECK(took < 10);
    CHECK_EQ(Json::parse(text_of(slow))["stdout"].get<std::string>(), "");

    // A command that needs a shell built-in and a pipeline keeps working afterwards.
    CHECK(!is_error(call(server, session, "mcp-bash", shell("true"))));

    // A missing command is a tool failure, not a protocol error.
    const Json nothing = call(server, session, "mcp-bash", Json::object());
    CHECK(nothing.contains("result"));
    CHECK(is_error(nothing));
    CHECK_HAS(text_of(nothing), "command");
}

E2E_CASE(tools_off) {
    TempDir root;
    Server off(free_port(), {"--quiet", "--no-file-tools", "--no-bash", "--fs-root", root.path()});
    const std::string session = initialize(off);

    const Json listed = off.request(rpc("/mcp", envelope("tools/list", Json(), 2), session)).json();
    const Json names = listed["result"]["tools"];
    CHECK_EQ(std::to_string(names.size()), "4");
    Json by_name = Json::array();
    for (const Json& tool : names) by_name.push_back(tool["name"]);
    CHECK_EQ(by_name.dump(),
             Json::array({"mcp-hello", "mcp-echo", "mcp-utc_now", "mcp-whoami"}).dump());
    CHECK_EQ(off.request(get("/health")).json()["tools"].dump(), by_name.dump());

    Json message;
    message["message"] = "still here";
    CHECK_EQ(text_of(call(off, session, "mcp-echo", message)), "still here");
    CHECK(call(off, session, "mcp-read", read_of("x")).contains("error"));
    CHECK(call(off, session, "mcp-bash", shell("true")).contains("error"));
    CHECK_EQ(off.request(rpc("/mcp", envelope("tools/call", tool_call("mcp-read", read_of("x")), 3), session))
                 .json()["error"]["data"]["available"]
                 .size(),
             4u);

    Server no_bash(free_port(), {"--quiet", "--no-bash", "--fs-root", root.path()});
    const std::string no_bash_session = initialize(no_bash);
    const Json seven =
        no_bash.request(rpc("/mcp", envelope("tools/list", Json(), 2), no_bash_session)).json();
    CHECK_EQ(std::to_string(seven["result"]["tools"].size()), "7");
    CHECK_HAS(seven["result"]["tools"].dump(), "mcp-edit");
    CHECK(seven["result"]["tools"].dump().find("mcp-bash") == std::string::npos);
    CHECK(call(no_bash, no_bash_session, "mcp-bash", shell("true")).contains("error"));
    CHECK(!is_error(call(no_bash, no_bash_session, "mcp-write", write_of("x.txt", "x"))));
}
