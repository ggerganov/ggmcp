// Backend-agnostic JSON value used by every layer above the JSON backend.
//
// No type from a JSON backend (nlohmann::json and friends) is visible here or
// anywhere above this header. See gg/json_backend.hpp for the backend interface.

#pragma once

#include <initializer_list>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace gg {

namespace json { struct IValue; }

class Json {
public:

    enum class Kind : unsigned char { Null, Bool, Int, Double, String, Array, Object };

    Json();
    Json(std::nullptr_t); // NOLINT: implicit by design
    Json(bool value); // NOLINT: implicit by design
    Json(int value); // NOLINT: implicit by design
    Json(long long value); // NOLINT: implicit by design
    Json(double value); // NOLINT: implicit by design
    Json(const char* value); // NOLINT: implicit by design
    Json(std::string value); // NOLINT: implicit by design
    Json(std::string_view value); // NOLINT: implicit by design
    Json(const Json& other);
    Json(Json&& other) noexcept;
    Json& operator=(const Json& other);
    Json& operator=(Json&& other) noexcept;
    ~Json();

    static Json null();
    static Json array();
    static Json object();
    static Json object(std::initializer_list<std::pair<const std::string, Json>> members);

    // Returns false and fills `error` instead of throwing when the text is not valid JSON.
    static bool tryParse(std::string_view text, Json& out, std::string* error = nullptr);
    static Json parse(std::string_view text);

    Kind kind() const;
    bool isNull() const;
    bool isBool() const;
    bool isInt() const;
    bool isDouble() const;
    bool isNumber() const;
    bool isString() const;
    bool isArray() const;
    bool isObject() const;
    std::string kindName() const;

    // Conversions. A type mismatch yields the fallback instead of throwing.
    bool asBool(bool fallback = false) const;
    long long asInt(long long fallback = 0) const;
    double asDouble(double fallback = 0.0) const;
    std::string asString(const std::string& fallback = {}) const;

    // Object access. Missing keys and out of range indexes read back as null.
    bool contains(std::string_view key) const;
    Json at(std::string_view key) const;
    Json at(std::size_t index) const;
    Json operator[](std::string_view key) const { return at(key); }
    Json operator[](const char* key) const { return at(key); }
    Json operator[](std::size_t index) const { return at(index); }
    Json operator[](int index) const { return at(static_cast<std::size_t>(index)); }
    void set(std::string key, Json value);
    void erase(std::string_view key);
    void push_back(Json value);

    // Member lookups that also tolerate a missing or wrongly typed member.
    std::string getString(std::string_view key, const std::string& fallback = {}) const;
    long long getInt(std::string_view key, long long fallback = 0) const;
    bool getBool(std::string_view key, bool fallback = false) const;
    bool hasText(std::string_view key) const;

    std::size_t size() const;
    bool empty() const;
    std::vector<std::string> keys() const;
    std::vector<Json> items() const;

    std::string dump(int indent = -1) const;
    bool equals(const Json& other) const;

private:
    explicit Json(std::unique_ptr<json::IValue> value);
    static std::unique_ptr<json::IValue> release(Json value);

    std::unique_ptr<json::IValue> value_;
};

bool operator==(const Json& lhs, const Json& rhs);

} // namespace gg
