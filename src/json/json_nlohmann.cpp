// nlohmann/json backend.
//
// The only translation unit in the tree that includes a JSON backend header.
// nlohmann types stop here: everything above talks to gg::Json.

#include "gg/json_backend.hpp"

#include <nlohmann/json.hpp>

#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace gg::json {
namespace {

using Value = nlohmann::json;

Json::Kind kindOf(const Value& value) {
    switch (value.type()) {
    case Value::value_t::null: return Json::Kind::Null;
    case Value::value_t::boolean: return Json::Kind::Bool;
    case Value::value_t::string: return Json::Kind::String;
    case Value::value_t::array: return Json::Kind::Array;
    case Value::value_t::object: return Json::Kind::Object;
    default:
        return value.is_number_float() ? Json::Kind::Double : Json::Kind::Int;
    }
}

class NlohmannValue final : public IValue {
public:
    explicit NlohmannValue(Value value) : value_(std::move(value)) {}

    std::unique_ptr<IValue> clone() const override {
        return std::make_unique<NlohmannValue>(value_);
    }

    bool equals(const IValue& other) const override {
        const auto* otherValue = dynamic_cast<const NlohmannValue*>(&other);
        return otherValue && value_ == otherValue->value_;
    }

    Json::Kind kind() const override { return kindOf(value_); }

    bool asBool(bool fallback) const override {
        return value_.is_boolean() ? value_.get<bool>() : fallback;
    }

    long long asInt(long long fallback) const override {
        if (value_.is_number_integer() || value_.is_number_unsigned()) {
            return value_.get<long long>();
        }
        if (value_.is_number_float()) {
            return static_cast<long long>(value_.get<double>());
        }
        return fallback;
    }

    double asDouble(double fallback) const override {
        return value_.is_number() ? value_.get<double>() : fallback;
    }

    std::string toString(const std::string& fallback) const override {
        return value_.is_string() ? value_.get<std::string>() : fallback;
    }

    std::size_t size() const override { return value_.is_null() ? 0 : value_.size(); }

    bool contains(std::string_view key) const override {
        return value_.is_object() && value_.find(std::string(key)) != value_.end();
    }

    std::unique_ptr<IValue> find(std::string_view key) const override {
        if (!value_.is_object()) {
            return nullptr;
        }
        auto it = value_.find(std::string(key));
        return it == value_.end() ? nullptr : std::make_unique<NlohmannValue>(it.value());
    }

    std::unique_ptr<IValue> index(std::size_t index) const override {
        if (!value_.is_array() || index >= value_.size()) {
            return nullptr;
        }
        return std::make_unique<NlohmannValue>(value_[index]);
    }

    void set(std::string key, std::unique_ptr<IValue> value) override {
        if (!value_.is_object()) {
            return;
        }
        value_[key] = unwrap(std::move(value));
    }

    void erase(std::string_view key) override {
        if (value_.is_object()) {
            value_.erase(std::string(key));
        }
    }

    void pushBack(std::unique_ptr<IValue> value) override {
        if (value_.is_array()) {
            value_.push_back(unwrap(std::move(value)));
        }
    }

    std::vector<std::string> keys() const override {
        std::vector<std::string> out;
        if (value_.is_object()) {
            out.reserve(value_.size());
            for (auto it = value_.begin(); it != value_.end(); ++it) {
                out.push_back(it.key());
            }
        }
        return out;
    }

    std::vector<std::unique_ptr<IValue>> items() const override {
        std::vector<std::unique_ptr<IValue>> out;
        if (value_.is_array()) {
            out.reserve(value_.size());
            for (const auto& item : value_) {
                out.push_back(std::make_unique<NlohmannValue>(item));
            }
        } else if (value_.is_object()) {
            out.reserve(value_.size());
            for (auto it = value_.begin(); it != value_.end(); ++it) {
                out.push_back(std::make_unique<NlohmannValue>(it.value()));
            }
        }
        return out;
    }

    std::string dump(int indent) const override {
        return value_.dump(indent, ' ', false, Value::error_handler_t::replace);
    }

    const Value& raw() const { return value_; }

private:
    static Value unwrap(std::unique_ptr<IValue> value) {
        auto* wrapped = dynamic_cast<NlohmannValue*>(value.get());
        return wrapped ? wrapped->value_ : Value();
    }

    Value value_;
};

class NlohmannBackend final : public IBackend {
public:
    std::string_view name() const override { return "nlohmann"; }

    std::unique_ptr<IValue> makeNull() const override { return make(Value()); }
    std::unique_ptr<IValue> makeBool(bool value) const override { return make(value); }
    std::unique_ptr<IValue> makeInt(long long value) const override { return make(value); }
    std::unique_ptr<IValue> makeDouble(double value) const override { return make(value); }
    std::unique_ptr<IValue> makeString(std::string value) const override {
        return make(std::move(value));
    }
    std::unique_ptr<IValue> makeArray() const override { return make(Value::array()); }
    std::unique_ptr<IValue> makeObject() const override { return make(Value::object()); }

    std::unique_ptr<IValue> parse(std::string_view text, std::string& error) const override {
        try {
            return make(Value::parse(text, nullptr, /*allow_exceptions=*/true,
                                     /*ignore_comments=*/false));
        } catch (const Value::exception& e) {
            error = e.what();
            return nullptr;
        } catch (const std::exception& e) {
            error = e.what();
            return nullptr;
        }
    }

private:
    template <typename T>
    static std::unique_ptr<IValue> make(T&& value) {
        return std::make_unique<NlohmannValue>(Value(std::forward<T>(value)));
    }
};

} // namespace

std::unique_ptr<IBackend> makeNlohmannBackend() {
    return std::make_unique<NlohmannBackend>();
}

} // namespace gg::json
