// The interface every JSON backend implements.
//
// gg::Json talks to a JSON backend only through these two interfaces, so a
// backend can be swapped by calling gg::json::setBackend() without touching any
// code above this header.

#pragma once

#include <cstddef>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "gg/json.hpp"

namespace gg::json {

// One JSON node owned by a backend.
struct IValue {
    virtual ~IValue() = default;

    virtual std::unique_ptr<IValue> clone() const = 0;
    virtual bool equals(const IValue& other) const = 0;

    virtual Json::Kind kind() const = 0;
    virtual bool asBool(bool fallback) const = 0;
    virtual long long asInt(long long fallback) const = 0;
    virtual double asDouble(double fallback) const = 0;
    virtual std::string toString(const std::string& fallback) const = 0;

    virtual std::size_t size() const = 0;
    virtual bool contains(std::string_view key) const = 0;
    virtual std::unique_ptr<IValue> find(std::string_view key) const = 0;
    virtual std::unique_ptr<IValue> index(std::size_t index) const = 0;
    virtual void set(std::string key, std::unique_ptr<IValue> value) = 0;
    virtual void erase(std::string_view key) = 0;
    virtual void pushBack(std::unique_ptr<IValue> value) = 0;
    virtual std::vector<std::string> keys() const = 0;
    virtual std::vector<std::unique_ptr<IValue>> items() const = 0;

    virtual std::string dump(int indent) const = 0;
};

// Creates and parses backend owned values.
struct IBackend {
    virtual ~IBackend() = default;

    virtual std::string_view name() const = 0;

    virtual std::unique_ptr<IValue> makeNull() const = 0;
    virtual std::unique_ptr<IValue> makeBool(bool value) const = 0;
    virtual std::unique_ptr<IValue> makeInt(long long value) const = 0;
    virtual std::unique_ptr<IValue> makeDouble(double value) const = 0;
    virtual std::unique_ptr<IValue> makeString(std::string value) const = 0;
    virtual std::unique_ptr<IValue> makeArray() const = 0;
    virtual std::unique_ptr<IValue> makeObject() const = 0;

    // Returns nullptr and fills `error` when the text is not valid JSON.
    virtual std::unique_ptr<IValue> parse(std::string_view text, std::string& error) const = 0;
};

// The backend in use. See gg/backends.hpp for the backends that are built in.
IBackend& backend();
void setBackend(std::unique_ptr<IBackend> backend);

} // namespace gg::json
