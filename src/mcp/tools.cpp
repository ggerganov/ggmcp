#include "gg/mcp/tools.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <vector>

namespace gg::mcp {
namespace {

std::string trimCopy(const std::string& text) {
    const auto first = text.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) {
        return "";
    }
    const auto last = text.find_last_not_of(" \t\r\n");
    return text.substr(first, last - first + 1);
}

std::string iso8601Now() {
    const auto now = std::chrono::system_clock::now();
    std::time_t wall = std::chrono::system_clock::to_time_t(now);
    const auto millis = static_cast<long long>(
        std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count() %
        1000);
    std::tm utc {};
#if defined(_WIN32)
    gmtime_s(&utc, &wall);
#else
    gmtime_r(&wall, &utc);
#endif
    char stamp[32];
    std::snprintf(stamp, sizeof(stamp), "%04d-%02d-%02dT%02d:%02d:%02d.%03lldZ",
                  utc.tm_year + 1900, utc.tm_mon + 1, utc.tm_mday, utc.tm_hour, utc.tm_min,
                  utc.tm_sec, millis);
    return stamp;
}

long long epochMillisNow() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

Json stringSchema(const std::string& description) {
    return Json::object({{"type", "string"}, {"description", description}});
}

Json stringArray(const std::vector<std::string>& values) {
    Json array = Json::array();
    for (const auto& value : values) {
        array.push_back(value);
    }
    return array;
}

Json schema(const std::vector<std::pair<const char*, Json>>& members,
            const std::vector<std::string>& required) {
    Json properties = Json::object();
    for (const auto& member : members) {
        properties.set(member.first, member.second);
    }
    Json schema_ = Json::object();
    schema_.set("type", "object");
    schema_.set("properties", properties);
    schema_.set("additionalProperties", false);
    if (!required.empty()) {
        schema_.set("required", stringArray(required));
    }
    return schema_;
}

Json textBlock(std::string text) {
    return Json::object({{"type", "text"}, {"text", std::move(text)}});
}

} // namespace

ToolResult ToolResult::ok(std::string text) {
    ToolResult result;
    result.text.push_back(std::move(text));
    return result;
}

ToolResult ToolResult::error(std::string text) {
    ToolResult result;
    result.isError = true;
    result.text.push_back(std::move(text));
    return result;
}

Json ToolResult::toJson() const {
    Json content = Json::array();
    for (const auto& block : text) {
        content.push_back(textBlock(block));
    }
    if (content.empty()) {
        content.push_back(textBlock(""));
    }
    Json result = Json::object();
    result.set("content", content);
    result.set("isError", isError);
    return result;
}

void ToolRegistry::setAlias(const std::string& alias) {
    alias_ = alias;
}

std::string ToolRegistry::qualify(const std::string& name) const {
    return alias_.empty() ? name : alias_ + "-" + name;
}

bool ToolRegistry::carriesAlias(const std::string& published) const {
    if (alias_.empty()) {
        return true;
    }
    const std::string prefix = alias_ + "-";
    return published.size() > prefix.size() && published.compare(0, prefix.size(), prefix) == 0;
}

std::string ToolRegistry::bare(const std::string& published) const {
    if (alias_.empty() || !carriesAlias(published)) {
        return published;
    }
    return published.substr(alias_.size() + 1);
}

void ToolRegistry::add(Tool tool) {
    tools_.emplace(tool.name, std::move(tool));
}

bool ToolRegistry::has(const std::string& published) const {
    return find(published) != nullptr;
}

const Tool* ToolRegistry::find(const std::string& published) const {
    if (!carriesAlias(published)) {
        return nullptr;
    }
    auto it = tools_.find(bare(published));
    return it == tools_.end() ? nullptr : &it->second;
}

std::vector<std::string> ToolRegistry::names() const {
    std::vector<std::string> names;
    names.reserve(tools_.size());
    for (const auto& entry : tools_) {
        names.push_back(qualify(entry.first));
    }
    std::sort(names.begin(), names.end());
    return names;
}

std::size_t ToolRegistry::size() const {
    return tools_.size();
}

Json ToolRegistry::list() const {
    Json tools = Json::array();
    for (const auto& entry : tools_) {
        Json tool = Json::object();
        tool.set("name", qualify(entry.second.name));
        if (!entry.second.title.empty()) {
            tool.set("title", entry.second.title);
        }
        tool.set("description", entry.second.description);
        tool.set("inputSchema", entry.second.inputSchema);
        tools.push_back(tool);
    }
    return Json::object({{"tools", tools}});
}

void registerCoreTools(ToolRegistry& registry) {
    {
        Tool tool;
        tool.name = "hello";
        tool.title = "Hello";
        tool.description = "Greet someone. Pass loud=true to shout.";
        tool.group = "core";
        tool.inputSchema = schema(
            {{"name", stringSchema("Who to greet, defaults to 'world'")},
             {"loud", Json::object({{"type", "boolean"},
                                    {"description", "Upper case the greeting"}})}},
            {});
        tool.handler = [](const ToolContext&, const Json& arguments) {
            std::string name = trimCopy(arguments.getString("name", "world"));
            if (name.empty()) {
                name = "world";
            }
            std::string greeting = "Hello, " + name + "!";
            if (arguments.getBool("loud", false)) {
                for (char& c : greeting) {
                    c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
                }
            }
            return ToolResult::ok(greeting);
        };
        registry.add(std::move(tool));
    }
    {
        Tool tool;
        tool.name = "echo";
        tool.title = "Echo";
        tool.description = "Return the given message unchanged.";
        tool.group = "core";
        tool.inputSchema = schema({{"message", stringSchema("Text to send back")}}, {"message"});
        tool.handler = [](const ToolContext&, const Json& arguments) {
            if (!arguments.isObject()) {
                return ToolResult::error("echo: arguments must be an object");
            }
            if (!arguments.hasText("message")) {
                return ToolResult::error("echo: 'message' is required and must be a string");
            }
            return ToolResult::ok(arguments.at("message").asString());
        };
        registry.add(std::move(tool));
    }
    {
        Tool tool;
        tool.name = "utc_now";
        tool.title = "UTC now";
        tool.description = "Current UTC time as ISO-8601 and as epoch milliseconds.";
        tool.group = "core";
        tool.inputSchema = schema({}, {});
        tool.handler = [](const ToolContext&, const Json&) {
            Json body = Json::object({
                {"iso8601", iso8601Now()},
                {"epochMillis", epochMillisNow()},
            });
            return ToolResult::ok(body.dump());
        };
        registry.add(std::move(tool));
    }
    {
        Tool tool;
        tool.name = "whoami";
        tool.title = "Who am I";
        tool.description = "Describe the live session that is calling this tool.";
        tool.group = "core";
        tool.inputSchema = schema({}, {});
        tool.handler = [](const ToolContext& context, const Json&) {
            Json body = Json::object();
            body.set("session", context.session.isNull() ? Json() : context.session);
            body.set("transport", context.transport);
            body.set("remoteAddress", context.clientAddress);
            body.set("root", context.root);
            body.set("confined", context.confineToRoot);
            return ToolResult::ok(body.dump());
        };
        registry.add(std::move(tool));
    }
}

void registerFileTools(ToolRegistry& registry) {
    {
        Tool tool;
        tool.name = "read";
        tool.title = "Read";
        tool.description = "Read a text file under the root, optionally by line offset/limit.";
        tool.group = "file";
        tool.inputSchema = schema({{"path", stringSchema("File to read")},
                                   {"offset", Json::object({{"type", "integer"},
                                                            {"description", "First line, 1"}})},
                                   {"limit", Json::object({{"type", "integer"},
                                                           {"description", "Line count"}})}},
                                  {"path"});
        tool.handler = [](const ToolContext& context, const Json& arguments) {
            return readFileText(context, arguments);
        };
        registry.add(std::move(tool));
    }
    {
        Tool tool;
        tool.name = "write";
        tool.title = "Write";
        tool.description = "Create or replace a file under the root. Writes are atomic.";
        tool.group = "file";
        tool.inputSchema = schema({{"path", stringSchema("File to write")},
                                   {"content", stringSchema("Text to store")},
                                   {"append", Json::object({{"type", "boolean"},
                                                            {"description", "Append instead "
                                                                             "of replacing"}})}},
                                  {"path", "content"});
        tool.handler = [](const ToolContext& context, const Json& arguments) {
            return writeFileText(context, arguments);
        };
        registry.add(std::move(tool));
    }
    {
        Tool tool;
        tool.name = "edit";
        tool.title = "Edit";
        tool.description = "Apply exact text replacements. All edits apply or none do.";
        tool.group = "file";
        Json editSchema = Json::object({{"type", "object"},
                                        {"properties",
                                         Json::object({{"oldText", stringSchema("Text to find")},
                                                       {"newText",
                                                        stringSchema("Replacement text")}})},
                                        {"additionalProperties", false}});
        editSchema.set("required", stringArray({"oldText", "newText"}));
        tool.inputSchema = schema({{"path", stringSchema("File to edit")},
                                   {"edits", Json::object({{"type", "array"},
                                                           {"items", editSchema},
                                                           {"description", "Replacements"}})},
                                   {"dryRun", Json::object({{"type", "boolean"},
                                                            {"description", "Check without "
                                                                             "writing"}})}},
                                  {"path", "edits"});
        tool.handler = [](const ToolContext& context, const Json& arguments) {
            return editFileText(context, arguments);
        };
        registry.add(std::move(tool));
    }
}

void registerBashTool(ToolRegistry& registry, int timeoutSeconds) {
    Tool tool;
    tool.name = "bash";
    tool.title = "Bash";
    tool.description = "Run a command with /bin/sh -c and a timeout. Not confined by the root.";
    tool.group = "shell";
    tool.inputSchema = schema({{"command", stringSchema("Command line to run")},
                               {"timeout", Json::object({{"type", "integer"},
                                                         {"description", "Timeout in seconds, "
                                                                          "default " +
                                                                              std::to_string(
                                                                                  timeoutSeconds)}})}},
                              {"command"});
    tool.handler = [timeoutSeconds](const ToolContext&, const Json& arguments) {
        return runShellTool(arguments, timeoutSeconds);
    };
    registry.add(std::move(tool));
}

void registerBuiltins(ToolRegistry& registry, const Options& options) {
    registerCoreTools(registry);
    if (options.fileTools) {
        registerFileTools(registry);
    }
    if (options.bashTool) {
        registerBashTool(registry, options.bashTimeoutSeconds);
    }
}

} // namespace gg::mcp
