// ggmcp - a tiny Model Context Protocol server built on vendored single-header libraries.
//
// json.hpp - the JSON type this project speaks.
//
// There is no bespoke JSON implementation here: Value *is* nlohmann::json
// (vendor/nlohmann/json.hpp), which owns parsing, serialization, escaping,
// number handling and the type checks. What this header adds is the small set
// of project conventions that nlohmann does not provide:
//
//   * Value / Array / Object aliases, so call sites read as project code and
//     the rest of the tree never has to spell out nlohmann::json.
//   * find(), returning a pointer instead of an iterator. At a protocol
//     boundary "absent" is a routine answer, and callers want to test it
//     rather than compare against end(), so nullptr means "no such member".
//   * parse() / try_parse(), which cap nesting depth *before* a tree is
//     built. nlohmann's parser is iterative (no stack risk on input), but its
//     serializer recurses, so a hostile message must be refused while it is
//     still text.

#ifndef GGGMCP_JSON_HPP
#define GGGMCP_JSON_HPP

#include <cstdint>
#include <map>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

namespace ggmcp {

/// The JSON value. Everything - parsing, dumping, typing - is nlohmann/json.
using Value = nlohmann::json;

/// Container types. These are exactly Value::array_t / Value::object_t, so
/// they convert to and from nlohmann containers without a copy.
using Array  = std::vector<Value>;
using Object = std::map<std::string, Value>;

/// Thrown by parse() on malformed input or on input that nests too deeply.
class parse_error : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

/// Deepest nesting parse() accepts. Deep enough for any real message, shallow
/// enough that dumping one never runs the recursive serializer out of stack.
inline constexpr int kMaxJsonDepth = 128;

/// Parses a complete JSON document. Throws ggmcp::parse_error on malformed
/// input, on trailing garbage, and on nesting deeper than `max_depth`.
Value parse(std::string_view input, int max_depth = kMaxJsonDepth);

/// Non-throwing parse. Returns false and fills `error` on failure, leaving
/// `out` untouched.
bool try_parse(std::string_view input, Value& out, std::string& error,
               int max_depth = kMaxJsonDepth);

/// Serialize to compact JSON. Identical to `value.dump()` except for invalid
/// UTF-8: nlohmann's default is to throw, which would turn "a tool returned a
/// byte that is not part of a character" into a failed request. ggmcp replaces
/// such bytes with U+FFFD instead, so the answer still goes out - as valid JSON.
std::string dump(const Value& value);

/// Object member lookup. Returns nullptr when the key is absent, or when
/// `value` is not an object - never throws, which is what makes it usable at
/// the protocol boundary.
const Value* find(const Value& value, std::string_view key);

/// Null-safe overload for chaining: `find(find(request, "params"), "name")`.
/// A missing member yields nullptr, and looking *into* nullptr yields nullptr
/// instead of a crash.
const Value* find(const Value* value, std::string_view key);

}  // namespace ggmcp

#endif  // GGGMCP_JSON_HPP
