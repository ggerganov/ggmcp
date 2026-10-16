#include "gg/backends.hpp"
#include "gg/json.hpp"
#include "gg/json_backend.hpp"

#include <cstring>
#include <memory>
#include <utility>

namespace gg {

namespace json {
namespace detail {

std::unique_ptr<IBackend>& backendSlot() {
    static std::unique_ptr<IBackend>* instance = new std::unique_ptr<IBackend>;
    return *instance;
}

} // namespace detail
} // namespace json

namespace {

json::IBackend& activeBackend() {
    auto& slot = json::detail::backendSlot();
    if (!slot) {
        std::string error;
        if (!backends::selectJsonBackend("nlohmann", error)) {
            throw std::runtime_error("no json backend installed: " + error);
        }
    }
    return *slot;
}

} // namespace

Json::Json() = default;

Json::Json(std::unique_ptr<json::IValue> value) : value_(std::move(value)) {}

Json::Json(std::nullptr_t) : value_(activeBackend().makeNull()) {}
Json::Json(bool value) : value_(activeBackend().makeBool(value)) {}
Json::Json(int value) : value_(activeBackend().makeInt(value)) {}
Json::Json(long long value) : value_(activeBackend().makeInt(value)) {}
Json::Json(double value) : value_(activeBackend().makeDouble(value)) {}
Json::Json(const char* value)
    : value_(activeBackend().makeString(value ? std::string(value) : std::string())) {}
Json::Json(std::string value) : value_(activeBackend().makeString(std::move(value))) {}
Json::Json(std::string_view value) : value_(activeBackend().makeString(std::string(value))) {}

Json::Json(const Json& other) : value_(other.value_ ? other.value_->clone() : nullptr) {}

Json::Json(Json&& other) noexcept = default;

Json& Json::operator=(const Json& other) {
    if (this != &other) {
        value_ = other.value_ ? other.value_->clone() : nullptr;
    }
    return *this;
}

Json& Json::operator=(Json&& other) noexcept = default;

Json::~Json() = default;

Json Json::null() { return Json(); }
Json Json::array() { return Json(activeBackend().makeArray()); }
Json Json::object() { return Json(activeBackend().makeObject()); }

Json Json::object(std::initializer_list<std::pair<const std::string, Json>> members) {
    Json out = Json::object();
    for (const auto& member : members) {
        out.set(member.first, member.second);
    }
    return out;
}

bool Json::tryParse(std::string_view text, Json& out, std::string* error) {
    std::string parseError;
    auto value = activeBackend().parse(text, parseError);
    if (!value) {
        if (error) {
            *error = parseError;
        }
        out = Json();
        return false;
    }
    if (error) {
        error->clear();
    }
    out = Json(std::move(value));
    return true;
}

Json Json::parse(std::string_view text) {
    Json out;
    std::string error;
    if (!tryParse(text, out, &error)) {
        throw std::runtime_error("json parse error: " + error);
    }
    return out;
}

Json::Kind Json::kind() const {
    return value_ ? value_->kind() : Kind::Null;
}

bool Json::isNull() const { return kind() == Kind::Null; }
bool Json::isBool() const { return kind() == Kind::Bool; }
bool Json::isInt() const { return kind() == Kind::Int; }
bool Json::isDouble() const { return kind() == Kind::Double; }
bool Json::isNumber() const { return isInt() || isDouble(); }
bool Json::isString() const { return kind() == Kind::String; }
bool Json::isArray() const { return kind() == Kind::Array; }
bool Json::isObject() const { return kind() == Kind::Object; }

std::string Json::kindName() const {
    switch (kind()) {
    case Kind::Null: return "null";
    case Kind::Bool: return "boolean";
    case Kind::Int: return "integer";
    case Kind::Double: return "number";
    case Kind::String: return "string";
    case Kind::Array: return "array";
    case Kind::Object: return "object";
    }
    return "unknown";
}

bool Json::asBool(bool fallback) const {
    return value_ ? value_->asBool(fallback) : fallback;
}

long long Json::asInt(long long fallback) const {
    return value_ ? value_->asInt(fallback) : fallback;
}

double Json::asDouble(double fallback) const {
    return value_ ? value_->asDouble(fallback) : fallback;
}

std::string Json::asString(const std::string& fallback) const {
    return value_ ? value_->toString(fallback) : fallback;
}

bool Json::contains(std::string_view key) const {
    return value_ && value_->contains(key);
}

Json Json::at(std::string_view key) const {
    if (!value_) {
        return Json();
    }
    auto found = value_->find(key);
    return found ? Json(std::move(found)) : Json();
}

Json Json::at(std::size_t index) const {
    if (!value_) {
        return Json();
    }
    auto found = value_->index(index);
    return found ? Json(std::move(found)) : Json();
}

std::unique_ptr<json::IValue> Json::release(Json value) {
    return std::move(value.value_);
}

void Json::set(std::string key, Json value) {
    if (!value_) {
        value_ = activeBackend().makeObject();
    }
    value_->set(std::move(key), release(std::move(value)));
}

void Json::erase(std::string_view key) {
    if (value_) {
        value_->erase(key);
    }
}

void Json::push_back(Json value) {
    if (!value_) {
        value_ = activeBackend().makeArray();
    }
    value_->pushBack(release(std::move(value)));
}

std::string Json::getString(std::string_view key, const std::string& fallback) const {
    Json member = at(key);
    return member.isString() ? member.asString(fallback) : fallback;
}

long long Json::getInt(std::string_view key, long long fallback) const {
    Json member = at(key);
    return member.isNumber() ? member.asInt(fallback) : fallback;
}

bool Json::getBool(std::string_view key, bool fallback) const {
    Json member = at(key);
    return member.isBool() ? member.asBool(fallback) : fallback;
}

bool Json::hasText(std::string_view key) const {
    return at(key).isString();
}

std::size_t Json::size() const {
    return value_ ? value_->size() : 0;
}

bool Json::empty() const {
    return size() == 0;
}

std::vector<std::string> Json::keys() const {
    return value_ ? value_->keys() : std::vector<std::string>{};
}

std::vector<Json> Json::items() const {
    std::vector<Json> out;
    if (!value_) {
        return out;
    }
    for (auto& item : value_->items()) {
        out.push_back(Json(std::move(item)));
    }
    return out;
}

std::string Json::dump(int indent) const {
    return value_ ? value_->dump(indent) : std::string("null");
}

bool Json::equals(const Json& other) const {
    if (!value_ || !other.value_) {
        return !value_ && !other.value_;
    }
    return value_->equals(*other.value_);
}

bool operator==(const Json& lhs, const Json& rhs) {
    return lhs.equals(rhs);
}

namespace json {

IBackend& backend() {
    return activeBackend();
}

void setBackend(std::unique_ptr<IBackend> backend) {
    detail::backendSlot() = std::move(backend);
}

} // namespace json

} // namespace gg
