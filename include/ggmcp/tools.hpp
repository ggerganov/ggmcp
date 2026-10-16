#pragma once

#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "ggmcp/json.hpp"

namespace ggmcp {

struct Config;

struct ToolContext {
    const Config* config = nullptr;
    std::string transport = "http";
    std::string address;
    std::string session_id;
};

struct ToolOutcome {
    bool ok = true;
    std::string text;
};

struct Tool {
    std::string bare_name;
    std::string title;
    std::string description;
    Json input_schema;
    std::function<ToolOutcome(const Json& args, const ToolContext& ctx)> run;
};

// Holds exactly the tools this instance publishes, under the names it publishes
// them with. `tools/call` accepts nothing this list does not carry.
class ToolRegistry {
public:
    explicit ToolRegistry(const Config& config);

    std::vector<std::string> names() const;
    const Tool* find(const std::string& published_name) const;
    Json published() const;
    const std::string& alias() const { return alias_; }

    ToolOutcome call(const std::string& published_name, const Json& args,
                     const ToolContext& ctx) const;

private:
    void add(Tool tool);

    const Config* config_;
    std::string alias_;
    std::vector<Tool> tools_;
};

ToolOutcome tool_error(const std::string& message);

}  // namespace ggmcp
