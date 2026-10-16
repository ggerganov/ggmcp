// Tool definitions and the tool registry.

#pragma once

#include <functional>
#include <map>
#include <string>
#include <vector>

#include "gg/json.hpp"
#include "gg/mcp/config.hpp"

namespace gg::mcp {

// What a tool is allowed to know about the request that invoked it.
struct ToolContext {
    std::string transport = "http";
    std::string clientAddress;
    std::string root;
    bool confineToRoot = true;
    Json session; // whoami view of the live session, null when stateless
};

struct ToolResult {
    bool isError = false;
    std::vector<std::string> text;

    static ToolResult ok(std::string text);
    static ToolResult error(std::string text);

    Json toJson() const; // MCP tools/call result
};

struct Tool {
    std::string name;
    std::string title;
    std::string description;
    std::string group = "core"; // core | file | shell
    Json inputSchema;
    std::function<ToolResult(const ToolContext&, const Json&)> handler;
};

// Tools are registered by their bare name and published as "<alias>-<name>".
// An empty alias publishes the bare names.
class ToolRegistry {
public:
    void setAlias(const std::string& alias);
    const std::string& alias() const { return alias_; }
    std::string qualify(const std::string& name) const;
    std::string bare(const std::string& published) const;

    void add(Tool tool);
    bool has(const std::string& published) const;
    const Tool* find(const std::string& published) const;
    std::vector<std::string> names() const; // published names, sorted
    std::size_t size() const;
    Json list() const; // {"tools": [...]}

private:
    bool carriesAlias(const std::string& published) const;

    std::map<std::string, Tool> tools_;
    std::string alias_;
};

void registerCoreTools(ToolRegistry& registry);
void registerFileTools(ToolRegistry& registry);
void registerBashTool(ToolRegistry& registry, int timeoutSeconds);
void registerBuiltins(ToolRegistry& registry, const Options& options);

// Confinement: resolve symlinks, then test containment against the root.
struct PathResolution {
    bool ok = false;
    std::string resolved;
    std::string error;
};

PathResolution confinePath(const std::string& root, const std::string& raw, bool allowOutside);

// File tool bodies, shared with the end to end tests.
ToolResult readFileText(const ToolContext& context, const Json& arguments);
ToolResult writeFileText(const ToolContext& context, const Json& arguments);
ToolResult editFileText(const ToolContext& context, const Json& arguments);

struct ShellResult {
    int exitCode = -1; // -1 when the command could not be started
    std::string output;
    std::string errorText;
    bool timedOut = false;
    long long durationMs = 0;
};

ShellResult runShell(const std::string& command, int timeoutSeconds);
ToolResult runShellTool(const Json& arguments, int timeoutSeconds);

} // namespace gg::mcp
