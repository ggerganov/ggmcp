#include "gg/mcp/tools.hpp"

#include <algorithm>
#include <cerrno>
#include <cstddef>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace gg::mcp {
namespace {

std::vector<std::string> splitLines(const std::string& text) {
    std::vector<std::string> lines;
    std::string current;
    for (const char c : text) {
        if (c == '\n') {
            lines.push_back(current);
            current.clear();
        } else {
            current.push_back(c);
        }
    }
    if (!current.empty()) {
        lines.push_back(current);
    }
    return lines;
}

std::string joinLines(const std::vector<std::string>& lines) {
    std::string text;
    for (const auto& line : lines) {
        text += line;
        text += '\n';
    }
    return text;
}

std::size_t countOccurrences(const std::string& haystack, const std::string& needle) {
    if (needle.empty()) {
        return 0;
    }
    std::size_t count = 0;
    std::size_t at = haystack.find(needle);
    while (at != std::string::npos) {
        ++count;
        at = haystack.find(needle, at + needle.size());
    }
    return count;
}

bool applyEdits(const std::string& original, const Json& edits, std::string& updated,
                std::size_t& replacements, std::string& error) {
    std::string text = original;
    std::size_t applied = 0;
    replacements = 0;

    for (std::size_t index = 0; index < edits.size(); ++index) {
        Json edit = edits[index];
        if (!edit.isObject()) {
            error = "edit " + std::to_string(index) + " must be an object";
            return false;
        }
        if (!edit.hasText("oldText")) {
            error = "edit " + std::to_string(index) + " needs a string 'oldText'";
            return false;
        }
        if (!edit.hasText("newText")) {
            error = "edit " + std::to_string(index) + " needs a string 'newText'";
            return false;
        }
        const std::string oldText = edit.at("oldText").asString();
        const std::string newText = edit.at("newText").asString();
        if (oldText.empty()) {
            error = "edit " + std::to_string(index) + " has an empty 'oldText'";
            return false;
        }
        const std::size_t hits = countOccurrences(text, oldText);
        if (hits == 0) {
            error = "edit " + std::to_string(index) + " found no match for its 'oldText'";
            return false;
        }
        std::string::size_type at = 0;
        for (std::size_t hit = 0; hit < hits; ++hit) {
            at = text.find(oldText, at);
            text.replace(at, oldText.size(), newText);
            at += newText.size();
        }
        ++applied;
        replacements += hits;
    }

    updated = std::move(text);
    return applied == edits.size();
}

bool writeAtomic(const std::string& path, const std::string& text, std::string& error) {
    std::error_code code;
    const fs::path target(path);
    if (target.has_parent_path() && !fs::exists(target.parent_path(), code)) {
        fs::create_directories(target.parent_path(), code);
    }
    const fs::path temporary = fs::path(path + ".ggmcp-tmp");
    {
        std::ofstream out(temporary, std::ios::binary | std::ios::trunc);
        if (!out) {
            error = "cannot open " + temporary.string() + " for writing";
            return false;
        }
        out.write(text.data(), static_cast<std::streamsize>(text.size()));
        out.flush();
        if (!out) {
            error = "cannot write " + temporary.string();
            return false;
        }
    }
    std::error_code renamed;
    fs::rename(temporary, target, renamed);
    if (renamed) {
        std::error_code removed;
        fs::remove(temporary, removed);
        error = "cannot replace " + path + ": " + renamed.message();
        return false;
    }
    return true;
}

bool readWholeFile(const std::string& path, std::string& text, std::string& error) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        error = "cannot read " + path;
        return false;
    }
    std::ostringstream buffer;
    buffer << in.rdbuf();
    text = buffer.str();
    return true;
}

std::string jsonError(const std::string& message) {
    return Json::object({{"error", message}}).dump();
}

} // namespace

PathResolution confinePath(const std::string& root, const std::string& raw, bool allowOutside) {
    PathResolution resolution;
    if (raw.empty()) {
        resolution.error = "path is required";
        return resolution;
    }
    if (raw.find('\0') != std::string::npos) {
        resolution.error = "path must not contain a NUL byte";
        return resolution;
    }

    std::error_code code;
    fs::path candidate(raw);
    if (!candidate.is_absolute()) {
        candidate = fs::path(root) / candidate;
    }

    fs::path resolved = fs::weakly_canonical(candidate, code);
    if (code || resolved.empty()) {
        resolution.error = "cannot resolve " + candidate.string() + ": " + code.message();
        return resolution;
    }

    if (!allowOutside) {
        fs::path base = fs::weakly_canonical(root, code);
        if (code || base.empty()) {
            resolution.error = "cannot resolve the root " + root + ": " + code.message();
            return resolution;
        }
        const fs::path relative = resolved.lexically_relative(base);
        if (relative.empty() || *relative.begin() == fs::path("..")) {
            resolution.error = "refusing to touch '" + raw + "', it is outside the root '" +
                               base.string() + "'";
            return resolution;
        }
    }

    resolution.ok = true;
    resolution.resolved = resolved.string();
    return resolution;
}

ToolResult readFileText(const ToolContext& context, const Json& arguments) {
    const std::string raw = arguments.getString("path");
    const PathResolution path = confinePath(context.root, raw, !context.confineToRoot);
    if (!path.ok) {
        return ToolResult::error(jsonError(path.error));
    }

    std::string text;
    std::string error;
    if (!readWholeFile(path.resolved, text, error)) {
        return ToolResult::error(jsonError(error));
    }

    const std::vector<std::string> lines = splitLines(text);
    const long long rawOffset = arguments.getInt("offset", 1);
    const long long offset = rawOffset < 1 ? 1 : rawOffset;
    const long long limit = arguments.getInt("limit", 0);

    const auto start = static_cast<std::size_t>(offset - 1);
    if (start > lines.size()) {
        return ToolResult::error(jsonError("offset " + std::to_string(offset) +
                                           " is past the end of " + std::to_string(lines.size()) +
                                           " lines"));
    }
    std::size_t stop = lines.size();
    if (limit > 0) {
        stop = std::min(lines.size(), start + static_cast<std::size_t>(limit));
    }

    const std::vector<std::string> selected(lines.begin() + static_cast<std::ptrdiff_t>(start),
                                            lines.begin() + static_cast<std::ptrdiff_t>(stop));
    return ToolResult::ok(joinLines(selected));
}

ToolResult writeFileText(const ToolContext& context, const Json& arguments) {
    const std::string raw = arguments.getString("path");
    const PathResolution path = confinePath(context.root, raw, !context.confineToRoot);
    if (!path.ok) {
        return ToolResult::error(jsonError(path.error));
    }
    if (!arguments.hasText("content")) {
        return ToolResult::error(jsonError("write needs a string 'content'"));
    }
    const std::string content = arguments.at("content").asString();
    const bool append = arguments.getBool("append", false);

    std::string text = content;
    bool created = false;
    std::error_code code;
    if (fs::exists(path.resolved, code)) {
        if (append) {
            std::string existing;
            std::string error;
            if (!readWholeFile(path.resolved, existing, error)) {
                return ToolResult::error(jsonError(error));
            }
            text = existing + content;
        }
    } else {
        created = true;
    }

    std::string error;
    if (!writeAtomic(path.resolved, text, error)) {
        return ToolResult::error(jsonError(error));
    }

    Json body = Json::object({{"path", path.resolved},
                              {"bytes", static_cast<long long>(content.size())},
                              {"total", static_cast<long long>(text.size())},
                              {"created", created},
                              {"appended", append}});
    return ToolResult::ok(body.dump());
}

ToolResult editFileText(const ToolContext& context, const Json& arguments) {
    const std::string raw = arguments.getString("path");
    const PathResolution path = confinePath(context.root, raw, !context.confineToRoot);
    if (!path.ok) {
        return ToolResult::error(jsonError(path.error));
    }
    const Json edits = arguments.at("edits");
    if (!edits.isArray() || edits.empty()) {
        return ToolResult::error(jsonError("edit needs a non-empty array 'edits'"));
    }

    std::string original;
    std::string error;
    if (!readWholeFile(path.resolved, original, error)) {
        return ToolResult::error(jsonError(error));
    }

    std::string updated;
    std::size_t replacements = 0;
    if (!applyEdits(original, edits, updated, replacements, error)) {
        return ToolResult::error(jsonError(error));
    }

    const bool dryRun = arguments.getBool("dryRun", false);
    if (!dryRun && !writeAtomic(path.resolved, updated, error)) {
        return ToolResult::error(jsonError(error));
    }

    Json body = Json::object({{"path", path.resolved},
                              {"edits", static_cast<long long>(edits.size())},
                              {"replacements", static_cast<long long>(replacements)},
                              {"bytes", static_cast<long long>(updated.size())},
                              {"dryRun", dryRun},
                              {"changed", updated != original}});
    return ToolResult::ok(body.dump());
}

} // namespace gg::mcp
