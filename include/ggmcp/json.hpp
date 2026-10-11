// ggmcp - a tiny Model Context Protocol server in dependency-free C++17.
//
// SPDX-License-Identifier: MIT
//
// json.hpp - a minimal RFC 8259 JSON value, parser and serializer.
//
// This exists only because C++ has no standard JSON. It is deliberately NOT a
// general-purpose library: no iterators, no custom allocators, no JSON Pointer,
// no schema validation. It parses and emits JSON-RPC 2.0 messages, correctly
// including UTF-8 and \uXXXX escapes, and nothing more.

#ifndef GGGMCP_JSON_HPP
#define GGGMCP_JSON_HPP

#include <cstdint>
#include <map>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <variant>
#include <vector>

namespace ggmcp {

class Value;

using Array  = std::vector<Value>;
using Object = std::map<std::string, Value>;

/// Thrown by Value::parse() on malformed input.
class parse_error : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

/// Thrown by the typed accessors when the dynamic type does not match.
class type_error : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

/// A JSON value.
class Value {
public:
    enum class Type { Null, Bool, Int, Double, String, Array, Object };

    Value() = default;

    static Value make_null()                     { return Value(); }
    static Value make_bool(bool v)               { Value x; x.data_ = v;            return x; }
    static Value make_int(std::int64_t v)        { Value x; x.data_ = v;            return x; }
    static Value make_double(double v)           { Value x; x.data_ = v;            return x; }
    static Value make_string(std::string v)      { Value x; x.data_ = std::move(v); return x; }
    static Value make_array(Array v = {})        { Value x; x.data_ = std::move(v); return x; }
    static Value make_object(Object v = {})      { Value x; x.data_ = std::move(v); return x; }

    /// Relies on the variant alternative order matching the Type enum order;
    /// asserted at the bottom of this class.
    Type type() const { return static_cast<Type>(data_.index()); }

    const char* type_name() const;

    bool is_null()   const { return std::holds_alternative<std::monostate>(data_); }
    bool is_bool()   const { return std::holds_alternative<bool>(data_); }
    bool is_int()    const { return std::holds_alternative<std::int64_t>(data_); }
    bool is_double() const { return std::holds_alternative<double>(data_); }
    bool is_number() const { return is_int() || is_double(); }
    bool is_string() const { return std::holds_alternative<std::string>(data_); }
    bool is_array()  const { return std::holds_alternative<Array>(data_); }
    bool is_object() const { return std::holds_alternative<Object>(data_); }

    bool as_bool() const                 { expect(is_bool());    return std::get<bool>(data_); }
    std::int64_t as_int() const          { expect(is_int());     return std::get<std::int64_t>(data_); }
    double as_double() const             { expect(is_number());  return is_int()
                                                                    ? static_cast<double>(std::get<std::int64_t>(data_))
                                                                    : std::get<double>(data_); }
    const std::string& as_string() const { expect(is_string());  return std::get<std::string>(data_); }
    const Array& as_array() const        { expect(is_array());   return std::get<Array>(data_); }
    const Object& as_object() const      { expect(is_object());  return std::get<Object>(data_); }
    Array& as_array()                    { expect(is_array());   return std::get<Array>(data_); }
    Object& as_object()                  { expect(is_object());  return std::get<Object>(data_); }

    /// Object lookup. Returns nullptr when absent or when *this is not an object.
    /// Never throws - this is the accessor you want at protocol boundaries.
    const Value* find(std::string_view key) const {
        if (!is_object()) return nullptr;
        const auto& obj = std::get<Object>(data_);
        const auto it = obj.find(std::string(key));
        return it == obj.end() ? nullptr : &it->second;
    }

    /// Array indexing. Returns nullptr when out of range or not an array.
    const Value* at(std::size_t index) const {
        if (!is_array()) return nullptr;
        const auto& arr = std::get<Array>(data_);
        return index < arr.size() ? &arr[index] : nullptr;
    }

    std::size_t size() const {
        if (is_array())  return std::get<Array>(data_).size();
        if (is_object()) return std::get<Object>(data_).size();
        if (is_string()) return std::get<std::string>(data_).size();
        return 0;
    }

    bool contains(std::string_view key) const { return find(key) != nullptr; }

    /// Serializes to compact JSON. UTF-8 safe; escapes only what RFC 8259 requires.
    void dump_to(std::string& out) const;
    std::string dump() const { std::string s; dump_to(s); return s; }

    /// Parses a complete JSON document. Throws parse_error on malformed input or
    /// trailing garbage. Nesting is capped to keep hostile input off the stack.
    static Value parse(std::string_view input, int max_depth = 128);

    /// Non-throwing parse. Returns false and fills `error` on failure.
    static bool try_parse(std::string_view input, Value& out, std::string& error, int max_depth = 128);

private:
    void expect(bool ok) const {
        if (!ok) throw type_error(std::string("json: wrong dynamic type, found ") + type_name());
    }

    std::variant<std::monostate, bool, std::int64_t, double, std::string, Array, Object> data_;

    static_assert(std::variant_size_v<decltype(data_)> == 7,
                  "Value::data_ alternatives must stay in lockstep with Value::Type");
};

/// Convenience shorthands so call sites read better than Value::make_string(...).
inline Value str(std::string s)  { return Value::make_string(std::move(s)); }
inline Value num(std::int64_t v) { return Value::make_int(v); }
inline Value boolean(bool v)     { return Value::make_bool(v); }

}  // namespace ggmcp

#endif  // GGGMCP_JSON_HPP
