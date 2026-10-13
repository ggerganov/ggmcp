// ggmcp - a tiny Model Context Protocol server built on vendored single-header libraries.
//
// json.cpp - the glue between this project's JSON conventions and
// nlohmann/json, which does the actual parsing and serialization.

#include "ggmcp/json.hpp"

namespace ggmcp {
namespace {

/// True when `input` nests deeper than `max_depth`.
///
/// This is a guard, not a parser: it looks only at brackets and skips string
/// literals (escapes included), which is exactly as much JSON as counting
/// nesting requires. Running it before nlohmann parses means a hostile message
/// is refused while it is still text - no tree is allocated, and a document
/// whose brackets all sit inside strings is never mistaken for a deep one.
bool nests_too_deep(std::string_view input, int max_depth) {
    int depth = 0;
    bool in_string = false;
    bool escaped = false;
    for (const char c : input) {
        if (in_string) {
            if (escaped) escaped = false;
            else if (c == '\\') escaped = true;
            else if (c == '"') in_string = false;
            continue;
        }
        if (c == '"') {
            in_string = true;
            continue;
        }
        if (c == '[' || c == '{') {
            if (++depth > max_depth) return true;
        } else if (c == ']' || c == '}') {
            if (depth > 0) --depth;
        }
    }
    return false;
}

}  // namespace

Value parse(std::string_view input, int max_depth) {
    if (max_depth < 1) max_depth = 1;
    if (nests_too_deep(input, max_depth)) {
        throw parse_error("json: input nests deeper than " + std::to_string(max_depth) +
                          " levels (depth limit is a guard against stack-deep documents, "
                          "not a comment on their validity)");
    }
    try {
        return Value::parse(input.begin(), input.end());
    } catch (const Value::parse_error& e) {
        // Hand the caller nlohmann's own message: it carries the line and
        // column, which is more than the old bespoke parser ever reported.
        throw parse_error(e.what());
    }
}

bool try_parse(std::string_view input, Value& out, std::string& error, int max_depth) {
    try {
        out = parse(input, max_depth);
        return true;
    } catch (const std::exception& e) {
        error = e.what();
        return false;
    }
}

std::string dump(const Value& value) {
    return value.dump(-1, ' ', false, Value::error_handler_t::replace);
}

const Value* find(const Value& value, std::string_view key) {
    if (!value.is_object()) return nullptr;
    const auto it = value.find(std::string(key));
    return it == value.end() ? nullptr : &*it;
}

const Value* find(const Value* value, std::string_view key) {
    return value == nullptr ? nullptr : find(*value, key);
}

}  // namespace ggmcp
