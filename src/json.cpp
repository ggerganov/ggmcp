// ggmcp - a tiny Model Context Protocol server in dependency-free C++17.
//
// SPDX-License-Identifier: MIT

#include "ggmcp/json.hpp"

#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>

namespace ggmcp {

const char* Value::type_name() const {
    switch (type()) {
        case Type::Null:   return "null";
        case Type::Bool:   return "boolean";
        case Type::Int:    return "integer";
        case Type::Double: return "number";
        case Type::String: return "string";
        case Type::Array:  return "array";
        case Type::Object: return "object";
    }
    return "unknown";
}

// ---------------------------------------------------------------------------
// Serialization
// ---------------------------------------------------------------------------

namespace {

void append_utf8(std::string& out, std::uint32_t cp) {
    if (cp < 0x80) {
        out.push_back(static_cast<char>(cp));
    } else if (cp < 0x800) {
        out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else if (cp < 0x10000) {
        out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else {
        out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    }
}

void dump_string(const std::string& s, std::string& out) {
    out.push_back('"');
    for (const char ch : s) {
        const auto c = static_cast<unsigned char>(ch);
        switch (c) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\b': out += "\\b";  break;
            case '\f': out += "\\f";  break;
            case '\n': out += "\\n";  break;
            case '\r': out += "\\r";  break;
            case '\t': out += "\\t";  break;
            default:
                if (c < 0x20) {
                    char buf[8];
                    std::snprintf(buf, sizeof(buf), "\\u%04x", c);
                    out += buf;
                } else {
                    // Bytes >= 0x20 (including UTF-8 continuation bytes) are legal
                    // raw in JSON. Passing them through keeps non-ASCII payloads
                    // intact instead of exploding them into \uXXXX sequences.
                    out.push_back(ch);
                }
        }
    }
    out.push_back('"');
}

void dump_double(double d, std::string& out) {
    if (!std::isfinite(d)) {
        out += "null";  // JSON has no literal for NaN/Infinity; null is the safe loss.
        return;
    }
    char buf[64];
    int written = 0;
    for (int precision = 15; precision <= 17; ++precision) {
        written = std::snprintf(buf, sizeof(buf), "%.*g", precision, d);
        if (written > 0 && std::strtod(buf, nullptr) == d) break;
    }
    if (written > 0) out.append(buf, static_cast<std::size_t>(written));
    else             out += "0";
}

}  // namespace

void Value::dump_to(std::string& out) const {
    switch (type()) {
        case Type::Null:   out += "null";  break;
        case Type::Bool:   out += as_bool() ? "true" : "false"; break;
        case Type::Int:    out += std::to_string(as_int());     break;
        case Type::Double: dump_double(as_double(), out);       break;
        case Type::String: dump_string(as_string(), out);       break;
        case Type::Array: {
            out.push_back('[');
            bool first = true;
            for (const auto& item : as_array()) {
                if (!first) out.push_back(',');
                first = false;
                item.dump_to(out);
            }
            out.push_back(']');
            break;
        }
        case Type::Object: {
            out.push_back('{');
            bool first = true;
            for (const auto& kv : as_object()) {
                if (!first) out.push_back(',');
                first = false;
                dump_string(kv.first, out);
                out.push_back(':');
                kv.second.dump_to(out);
            }
            out.push_back('}');
            break;
        }
    }
}

// ---------------------------------------------------------------------------
// Parsing
// ---------------------------------------------------------------------------

namespace {

class Parser {
public:
    Parser(std::string_view input, int max_depth) : in_(input), max_depth_(max_depth < 1 ? 1 : max_depth) {}

    Value parse_document() {
        skip_ws();
        Value v = parse_value();
        skip_ws();
        if (pos_ != in_.size()) fail("trailing characters after document");
        return v;
    }

private:
    [[noreturn]] void fail(const std::string& what) const {
        throw parse_error("json: " + what + " (offset " + std::to_string(pos_) + ")");
    }

    bool eof() const { return pos_ >= in_.size(); }

    char peek() const {
        if (eof()) fail("unexpected end of input");
        return in_[pos_];
    }

    char advance() {
        if (eof()) fail("unexpected end of input");
        return in_[pos_++];
    }

    void skip_ws() {
        while (!eof()) {
            switch (in_[pos_]) {
                case ' ': case '\t': case '\n': case '\r': ++pos_; break;
                default: return;
            }
        }
    }

    void expect(char c) {
        if (eof() || in_[pos_] != c) fail(std::string("expected '") + c + "'");
        ++pos_;
    }

    bool literal(const char* lit) {
        const std::size_t n = std::strlen(lit);
        if (in_.compare(pos_, n, lit) == 0) {
            pos_ += n;
            return true;
        }
        return false;
    }

    void parse_hex_escape(std::string& out) {
        std::uint32_t cp = 0;
        for (int i = 0; i < 4; ++i) {
            const char c = advance();
            cp <<= 4;
            if (c >= '0' && c <= '9')      cp |= static_cast<std::uint32_t>(c - '0');
            else if (c >= 'a' && c <= 'f') cp |= static_cast<std::uint32_t>(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F') cp |= static_cast<std::uint32_t>(c - 'A' + 10);
            else fail("invalid \\u escape");
        }
        if (cp >= 0xD800 && cp <= 0xDBFF) {  // high surrogate: must pair with a low one
            if (advance() != '\\' || advance() != 'u') fail("unpaired high surrogate");
            std::uint32_t low = 0;
            for (int i = 0; i < 4; ++i) {
                const char c = advance();
                low <<= 4;
                if (c >= '0' && c <= '9')      low |= static_cast<std::uint32_t>(c - '0');
                else if (c >= 'a' && c <= 'f') low |= static_cast<std::uint32_t>(c - 'a' + 10);
                else if (c >= 'A' && c <= 'F') low |= static_cast<std::uint32_t>(c - 'A' + 10);
                else fail("invalid \\u escape in surrogate pair");
            }
            if (low < 0xDC00 || low > 0xDFFF) fail("invalid low surrogate");
            const std::uint32_t combined = 0x10000 + ((cp - 0xD800) << 10) + (low - 0xDC00);
            ggmcp::append_utf8(out, combined);
            return;
        }
        if (cp >= 0xDC00 && cp <= 0xDFFF) fail("unpaired low surrogate");
        ggmcp::append_utf8(out, cp);
    }

    std::string parse_string() {
        expect('"');
        std::string s;
        for (;;) {
            const char c = advance();
            if (c == '"') return s;
            if (c != '\\') {
                if (static_cast<unsigned char>(c) < 0x20) fail("raw control character in string");
                s.push_back(c);
                continue;
            }
            const char e = advance();
            switch (e) {
                case '"':  s.push_back('"');  break;
                case '\\': s.push_back('\\'); break;
                case '/':  s.push_back('/');  break;
                case 'b':  s.push_back('\b'); break;
                case 'f':  s.push_back('\f'); break;
                case 'n':  s.push_back('\n'); break;
                case 'r':  s.push_back('\r'); break;
                case 't':  s.push_back('\t'); break;
                case 'u':  parse_hex_escape(s); break;
                default:   fail("unknown escape \\" + std::string(1, e));
            }
        }
    }

    /// Strict RFC 8259 number grammar: minus? (0 | [1-9][0-9]*) ('.' [0-9]+)?
    /// ([eE] [+-]? [0-9]+)?  This is what rejects ".5", "+1", "01" and "1."
    /// instead of letting strtod quietly accept them.
    Value parse_number() {
        const auto is_digit = [](char c) { return c >= '0' && c <= '9'; };
        const std::size_t start = pos_;
        bool real = false;

        if (peek() == '-') ++pos_;
        if (eof() || !is_digit(peek())) fail("expected a digit in number");
        if (peek() == '0') {
            ++pos_;
            if (!eof() && is_digit(peek())) fail("leading zero in number");
        } else {
            while (!eof() && is_digit(peek())) ++pos_;
        }

        if (!eof() && peek() == '.') {
            real = true;
            ++pos_;
            if (eof() || !is_digit(peek())) fail("expected digits after the decimal point");
            while (!eof() && is_digit(peek())) ++pos_;
        }

        if (!eof() && (peek() == 'e' || peek() == 'E')) {
            real = true;
            ++pos_;
            if (!eof() && (peek() == '+' || peek() == '-')) ++pos_;
            if (eof() || !is_digit(peek())) fail("expected digits in the exponent");
            while (!eof() && is_digit(peek())) ++pos_;
        }

        const std::string text(in_.substr(start, pos_ - start));
        const char* begin = text.c_str();
        char* end = nullptr;

        if (!real) {
            errno = 0;
            const long long integer = std::strtoll(begin, &end, 10);
            if (errno == ERANGE) {
                // Out of int64 range: keep the magnitude as a double rather than
                // wrapping to a wrong integer.
                return Value::make_double(std::strtod(begin, nullptr));
            }
            return Value::make_int(static_cast<std::int64_t>(integer));
        }

        const double number = std::strtod(begin, &end);
        if (!end || *end != '\0') fail("malformed number");
        return Value::make_double(number);
    }

    Value parse_value() {
        if (++depth_ > max_depth_) fail("nesting too deep");
        struct Guard {
            int& d;
            ~Guard() { --d; }
        } guard{depth_};

        switch (peek()) {
            case '{': return parse_object();
            case '[': return parse_array();
            case '"': return Value::make_string(parse_string());
            case 't': if (literal("true"))  return Value::make_bool(true);  fail("invalid literal");
            case 'f': if (literal("false")) return Value::make_bool(false); fail("invalid literal");
            case 'n': if (literal("null"))  return Value::make_null();      fail("invalid literal");
            default:  return parse_number();
        }
    }

    Value parse_object() {
        expect('{');
        Object obj;
        skip_ws();
        if (peek() == '}') { advance(); return Value::make_object(std::move(obj)); }
        for (;;) {
            skip_ws();
            std::string key = parse_string();
            skip_ws();
            expect(':');
            skip_ws();
            Value v = parse_value();
            if (!obj.emplace(std::move(key), std::move(v)).second) fail("duplicate object key");
            skip_ws();
            const char c = advance();
            if (c == '}') return Value::make_object(std::move(obj));
            if (c != ',') fail("expected ',' or '}' in object");
        }
    }

    Value parse_array() {
        expect('[');
        Array arr;
        skip_ws();
        if (peek() == ']') { advance(); return Value::make_array(std::move(arr)); }
        for (;;) {
            skip_ws();
            arr.push_back(parse_value());
            skip_ws();
            const char c = advance();
            if (c == ']') return Value::make_array(std::move(arr));
            if (c != ',') fail("expected ',' or ']' in array");
        }
    }

    std::string_view in_;
    std::size_t pos_ = 0;
    int max_depth_;
    int depth_ = 0;
};

}  // namespace

Value Value::parse(std::string_view input, int max_depth) {
    return Parser(input, max_depth).parse_document();
}

bool Value::try_parse(std::string_view input, Value& out, std::string& error, int max_depth) {
    try {
        out = parse(input, max_depth);
        return true;
    } catch (const std::exception& e) {
        error = e.what();
        return false;
    }
}

}  // namespace ggmcp
