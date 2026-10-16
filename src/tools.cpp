#include "ggmcp/tools.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <sstream>
#include <vector>

#include "ggmcp/config.hpp"
#include "ggmcp/paths.hpp"
#include "ggmcp/shell.hpp"

namespace ggmcp {
namespace {

ToolOutcome fail(const std::string& message) { return tool_error(message); }

bool want_string(const Json& args, const std::string& key, bool required, const std::string& bare,
                 std::string& out, std::string& error) {
    const auto it = args.find(key);
    if (it == args.end() || it->is_null()) {
        if (!required) return true;
        error = bare + ": \"" + key + "\" is required";
        return false;
    }
    if (!it->is_string()) {
        error = bare + ": \"" + key + "\" must be a string";
        return false;
    }
    out = it->get<std::string>();
    return true;
}

bool want_bool(const Json& args, const std::string& key, const std::string& bare, bool& out,
               std::string& error) {
    const auto it = args.find(key);
    if (it == args.end() || it->is_null()) return true;
    if (!it->is_boolean()) {
        error = bare + ": \"" + key + "\" must be a boolean";
        return false;
    }
    out = it->get<bool>();
    return true;
}

bool want_int(const Json& args, const std::string& key, long long fallback, long long low,
              long long high, const std::string& bare, const std::string& what, long long& out,
              std::string& error) {
    const auto it = args.find(key);
    if (it == args.end() || it->is_null()) {
        out = fallback;
        return true;
    }
    const long long value = it->is_number_integer() ? it->get<long long>() : -1;
    if (!it->is_number_integer() || value < low || value > high) {
        error = bare + ": \"" + key + "\" " + what;
        return false;
    }
    out = value;
    return true;
}

// An empty `required` is not valid JSON Schema, and some clients refuse it, so
// the key is left out when there is nothing to require.
Json object_schema(Json properties, std::vector<std::string> required) {
    Json schema;
    schema["additionalProperties"] = false;
    schema["properties"] = properties.is_null() ? Json::object() : properties;
    if (!required.empty()) schema["required"] = required;
    schema["type"] = "object";
    return schema;
}

Json string_property(const std::string& description) {
    Json property;
    property["description"] = description;
    property["type"] = "string";
    return property;
}

Json boolean_property(const std::string& description) {
    Json property;
    property["description"] = description;
    property["type"] = "boolean";
    return property;
}

Json integer_property(const std::string& description) {
    Json property;
    property["description"] = description;
    property["type"] = "integer";
    return property;
}

std::string upper(std::string text) {
    for (char& c : text) {
        if (c >= 'a' && c <= 'z') c = static_cast<char>(c - 'a' + 'A');
    }
    return text;
}

std::vector<std::string> split_lines(const std::string& text) {
    std::vector<std::string> lines;
    std::string line;
    for (char c : text) {
        if (c == '\n') {
            lines.push_back(line);
            line.clear();
        } else {
            line += c;
        }
    }
    if (!line.empty()) lines.push_back(line);
    return lines;
}

std::string joined(const std::vector<std::string>& lines, std::size_t begin, std::size_t count) {
    std::ostringstream out;
    for (std::size_t i = 0; i < count; ++i) {
        if (i != 0) out << '\n';
        out << lines[begin + i];
    }
    return out.str();
}

ToolOutcome run_hello(const Json& args, const ToolContext&) {
    std::string problem;
    std::string name = "world";
    if (!want_string(args, "name", false, "hello", name, problem)) return fail(problem);
    bool loud = false;
    if (!want_bool(args, "loud", "hello", loud, problem)) return fail(problem);
    const std::string text = "Hello, " + (name.empty() ? std::string("world") : name) + "!";
    return ToolOutcome{true, loud ? upper(text) : text};
}

ToolOutcome run_echo(const Json& args, const ToolContext&) {
    std::string problem;
    std::string message;
    if (!want_string(args, "message", true, "echo", message, problem)) return fail(problem);
    return ToolOutcome{true, message};
}

ToolOutcome run_utc_now(const Json&, const ToolContext&) {
    const auto millis = std::chrono::duration_cast<std::chrono::milliseconds>(
                            std::chrono::system_clock::now().time_since_epoch())
                            .count();
    const std::time_t seconds = static_cast<std::time_t>(millis / 1000);
    const long fraction = static_cast<long>(millis % 1000);

    std::tm utc {};
#if defined(_WIN32)
    gmtime_s(&utc, &seconds);
#else
    gmtime_r(&seconds, &utc);
#endif
    char stamp[32] = {};
    std::strftime(stamp, sizeof(stamp), "%Y-%m-%dT%H:%M:%S", &utc);

    char iso[40] = {};
    std::snprintf(iso, sizeof(iso), "%s.%03ldZ", stamp, fraction);

    Json body;
    body["epochMillis"] = millis;
    body["iso8601"] = std::string(iso);
    return ToolOutcome{true, dump(body)};
}

ToolOutcome run_whoami(const Json&, const ToolContext& ctx) {
    const PathPolicy policy = PathPolicy::for_config(*ctx.config);
    Json body;
    body["address"] = ctx.address;
    body["confined"] = policy.confined;
    body["root"] = policy.confined ? Json(policy.root) : Json();
    body["session"] = ctx.session_id.empty() ? Json() : Json(ctx.session_id);
    body["transport"] = ctx.transport;
    return ToolOutcome{true, dump(body)};
}

ToolOutcome run_read(const Json& args, const ToolContext& ctx) {
    std::string problem;
    std::string requested;
    if (!want_string(args, "path", true, "read", requested, problem)) return fail(problem);
    long long offset = 1;
    if (!want_int(args, "offset", 1, 1, 1LL << 40, "read", "wants a line number of at least 1",
                  offset, problem)) {
        return fail(problem);
    }
    long long limit = 0;
    if (!want_int(args, "limit", 0, 0, 1LL << 40, "read", "wants a line count of at least 0",
                  limit, problem)) {
        return fail(problem);
    }

    const PathPolicy policy = PathPolicy::for_config(*ctx.config);
    std::string path;
    std::string error;
    if (!policy.resolve(requested, true, path, error)) return fail("read: " + error);

    std::string content;
    if (!read_file(path, content, error)) return fail("read: " + error);

    std::string text = content;
    if (offset != 1) {
        std::size_t at = 0;
        for (long long line = 1; line < offset && at <= content.size(); ++line) {
            const std::size_t next = content.find('\n', at);
            if (next == std::string::npos) {
                at = content.size();
                break;
            }
            at = next + 1;
        }
        text = content.substr(std::min<std::size_t>(at, content.size()));
    }
    if (limit != 0) {
        const std::vector<std::string> lines = split_lines(text);
        const std::size_t count = std::min<std::size_t>(static_cast<std::size_t>(limit), lines.size());
        text = joined(lines, 0, count);
    }
    return ToolOutcome{true, text};
}

ToolOutcome run_write(const Json& args, const ToolContext& ctx) {
    std::string problem;
    std::string requested;
    if (!want_string(args, "path", true, "write", requested, problem)) return fail(problem);
    std::string content;
    if (!want_string(args, "content", true, "write", content, problem)) return fail(problem);
    bool append = false;
    if (!want_bool(args, "append", "write", append, problem)) return fail(problem);

    const PathPolicy policy = PathPolicy::for_config(*ctx.config);
    std::string path;
    std::string error;
    if (!policy.resolve(requested, false, path, error)) return fail("write: " + error);

    const bool ok = append ? append_file(path, content, error) : write_file(path, content, error);
    if (!ok) return fail("write: " + error);

    Json body;
    body["appended"] = append;
    body["bytes"] = content.size();
    body["path"] = path;
    return ToolOutcome{true, dump(body)};
}

ToolOutcome run_edit(const Json& args, const ToolContext& ctx) {
    std::string problem;
    std::string requested;
    if (!want_string(args, "path", true, "edit", requested, problem)) return fail(problem);
    bool dry_run = false;
    if (!want_bool(args, "dryRun", "edit", dry_run, problem)) return fail(problem);

    const auto edits = args.find("edits");
    if (edits == args.end() || !edits->is_array()) return fail("edit: \"edits\" must be an array");
    if (edits->empty()) return fail("edit: \"edits\" must hold at least one edit");

    const PathPolicy policy = PathPolicy::for_config(*ctx.config);
    std::string path;
    std::string error;
    if (!policy.resolve(requested, true, path, error)) return fail("edit: " + error);

    std::string text;
    if (!read_file(path, text, error)) return fail("edit: " + error);

    std::size_t replacements = 0;
    std::size_t index = 0;
    for (const Json& edit : *edits) {
        ++index;
        if (!edit.is_object()) {
            return fail("edit: edit " + std::to_string(index) + " must be an object");
        }
        std::string old_text;
        if (!want_string(edit, "oldText", true, "edit", old_text, problem)) {
            problem = "edit: edit " + std::to_string(index) + " wants \"oldText\"";
            return fail(problem);
        }
        if (old_text.empty()) {
            return fail("edit: \"oldText\" must not be empty");
        }
        std::string new_text;
        if (!want_string(edit, "newText", true, "edit", new_text, problem)) {
            problem = "edit: edit " + std::to_string(index) + " wants \"newText\"";
            return fail(problem);
        }
        bool all = false;
        if (!want_bool(edit, "replace", "edit", all, problem)) return fail(problem);

        std::size_t hits = 0;
        std::size_t at = text.find(old_text);
        while (at != std::string::npos) {
            ++hits;
            text.replace(at, old_text.size(), new_text);
            if (!all) break;
            at = text.find(old_text, at + new_text.size());
        }
        if (hits == 0) {
            return fail("edit: nothing matched " + dump(Json(old_text)));
        }
        replacements += hits;
    }

    if (!dry_run && !write_file(path, text, error)) return fail("edit: " + error);

    Json body;
    body["changed"] = !dry_run;
    body["dryRun"] = dry_run;
    body["path"] = path;
    body["replacements"] = replacements;
    return ToolOutcome{true, dump(body)};
}

ToolOutcome run_bash(const Json& args, const ToolContext&) {
    std::string problem;
    std::string command;
    if (!want_string(args, "command", true, "bash", command, problem)) return fail(problem);
    long long timeout = 60;
    if (!want_int(args, "timeout", 60, 1, 3600, "bash",
                  "wants a number of seconds between 1 and 3600", timeout, problem)) {
        return fail(problem);
    }

    const ShellResult ran = run_shell(command, static_cast<int>(timeout));
    if (!ran.started) return fail("bash: " + ran.error);

    Json body;
    body["command"] = command;
    body["exitCode"] = ran.exit_code;
    body["stderr"] = ran.err;
    body["stdout"] = ran.out;
    body["timedOut"] = ran.timed_out;
    const std::string text = dump_safely(body);

    if (ran.timed_out) {
        return ToolOutcome{false, text};
    }
    return ToolOutcome{ran.exit_code == 0, text};
}

}  // namespace

ToolOutcome tool_error(const std::string& message) {
    Json body;
    body["error"] = message;
    return ToolOutcome{false, dump_safely(body)};
}

ToolRegistry::ToolRegistry(const Config& config) : config_(&config), alias_(config.alias) {
    Tool hello;
    hello.bare_name = "hello";
    hello.title = "Hello";
    hello.description = "Greet someone.";
    hello.input_schema = object_schema(
        Json::object({{"name", string_property("Who to greet. Default: world.")},
                      {"loud", boolean_property("Shout the greeting.")}}),
        {});
    hello.run = run_hello;
    add(hello);

    Tool echo;
    echo.bare_name = "echo";
    echo.title = "Echo";
    echo.description = "Return a message unchanged.";
    echo.input_schema = object_schema(
        Json::object({{"message", string_property("What to send back.")}}), {"message"});
    echo.run = run_echo;
    add(echo);

    Tool utc_now;
    utc_now.bare_name = "utc_now";
    utc_now.title = "UTC now";
    utc_now.description = "The current UTC time, as ISO-8601 and as epoch milliseconds.";
    utc_now.input_schema = object_schema(Json::object(), {});
    utc_now.run = run_utc_now;
    add(utc_now);

    Tool whoami;
    whoami.bare_name = "whoami";
    whoami.title = "Who am I";
    whoami.description =
        "Describe the live session: the root, its confinement, the peer address, "
        "the transport and the session id.";
    whoami.input_schema = object_schema(Json::object(), {});
    whoami.run = run_whoami;
    add(whoami);

    if (config.file_tools) {
        Tool read;
        read.bare_name = "read";
        read.title = "Read";
        read.description =
            "Read a file under the root by line. The bytes found are the bytes returned.";
        read.input_schema = object_schema(
            Json::object({{"path", string_property("Path, relative to the root or absolute.")},
                          {"offset", integer_property("First line to read, 1 based.")},
                          {"limit", integer_property("How many lines to read. Default: the rest.")}}),
            {"path"});
        read.run = run_read;
        add(read);

        Tool write;
        write.bare_name = "write";
        write.title = "Write";
        write.description =
            "Create or replace a file. A replace is atomic; an append adds to the end.";
        write.input_schema = object_schema(
            Json::object({{"path", string_property("Path, relative to the root or absolute.")},
                          {"content", string_property("The bytes to write.")},
                          {"append", boolean_property("Append instead of replacing.")}}),
            {"path", "content"});
        write.run = run_write;
        add(write);

        Tool edit;
        edit.bare_name = "edit";
        edit.title = "Edit";
        edit.description =
            "Replace exact text in a file. Every edit applies, or none of them does. "
            "\"replace\" applies an edit to every occurrence rather than the first.";
        Json edit_item;
        edit_item["additionalProperties"] = false;
        edit_item["properties"] = Json::object({{"oldText", string_property("Text to find.")},
                                                {"newText", string_property("Text to put in its place.")},
                                                {"replace", boolean_property("Replace every occurrence.")}});
        edit_item["required"] = Json::array({"oldText", "newText"});
        edit_item["type"] = "object";
        Json edits;
        edits["description"] = "The edits to apply, in order.";
        edits["items"] = edit_item;
        edits["minItems"] = 1;
        edits["type"] = "array";
        edit.input_schema = object_schema(
            Json::object({{"path", string_property("Path, relative to the root or absolute.")},
                          {"edits", edits},
                          {"dryRun", boolean_property("Report what would change and write nothing.")}}),
            {"path", "edits"});
        edit.run = run_edit;
        add(edit);
    }

    if (config.bash_tool) {
        Tool bash;
        bash.bare_name = "bash";
        bash.title = "Bash";
        bash.description =
            "Run a command with /bin/sh -c and report stdout, stderr and the exit code. "
            "A non-zero exit is reported with isError set. Not confined by the root.";
        bash.input_schema = object_schema(
            Json::object({{"command", string_property("The command line to run.")},
                          {"timeout", integer_property("Seconds to wait. Default: 60.")}}),
            {"command"});
        bash.run = run_bash;
        add(bash);
    }
}

void ToolRegistry::add(Tool tool) { tools_.push_back(std::move(tool)); }

std::vector<std::string> ToolRegistry::names() const {
    std::vector<std::string> out;
    out.reserve(tools_.size());
    for (const Tool& tool : tools_) out.push_back(config_->tool_name(tool.bare_name));
    return out;
}

const Tool* ToolRegistry::find(const std::string& published_name) const {
    for (const Tool& tool : tools_) {
        if (config_->tool_name(tool.bare_name) == published_name) return &tool;
    }
    return nullptr;
}

Json ToolRegistry::published() const {
    Json tools = Json::array();
    for (const Tool& tool : tools_) {
        Json entry;
        entry["description"] = tool.description;
        entry["inputSchema"] = tool.input_schema;
        entry["name"] = config_->tool_name(tool.bare_name);
        entry["title"] = tool.title;
        tools.push_back(entry);
    }
    return tools;
}

ToolOutcome ToolRegistry::call(const std::string& published_name, const Json& args,
                               const ToolContext& ctx) const {
    const Tool* tool = find(published_name);
    if (tool == nullptr) return tool_error("unknown tool: " + published_name);
    return tool->run(args, ctx);
}

}  // namespace ggmcp
