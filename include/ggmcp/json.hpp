#pragma once

#include <nlohmann/json.hpp>

#include <string>

namespace ggmcp {

using Json = nlohmann::json;

inline std::string dump(const Json& value) { return value.dump(); }

// Tools hand back bytes they did not author: `read` returns whatever a file holds
// and `bash` whatever a command printed, and neither is obliged to be UTF-8.
inline std::string dump_safely(const Json& value) {
    return value.dump(-1, ' ', false, Json::error_handler_t::replace);
}

inline bool is_prefix(const std::string& haystack, const std::string& needle) {
    return haystack.size() >= needle.size() &&
           haystack.compare(0, needle.size(), needle) == 0;
}

}  // namespace ggmcp
