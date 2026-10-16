#pragma once

#include <stdexcept>
#include <string>
#include <type_traits>
#include <vector>

#include <nlohmann/json.hpp>

namespace e2e {

using Json = nlohmann::json;

struct Failure : std::runtime_error {
    explicit Failure(const std::string& what) : std::runtime_error(what) {}
};

struct Registrar {
    Registrar(const char* name, void (*run)());
};

std::vector<std::string> names();
int run_all(const std::string& only);

void fail(const char* file, int line, const std::string& what);

void check(bool ok, const char* file, int line, const std::string& what);
void check_eq_str(const std::string& got, const std::string& want, const char* file, int line,
                  const std::string& what);

inline std::string text(const std::string& value) { return value; }
inline std::string text(const char* value) { return std::string(value); }
template <typename Number>
inline std::string text(const Number& value) {
    return std::to_string(value);
}

template <typename A, typename B>
void check_eq(const A& got, const B& want, const char* file, int line, const std::string& what) {
    check_eq_str(text(got), text(want), file, line, what);
}
void check_has(const std::string& haystack, const std::string& needle, const char* file, int line,
               const std::string& what);

// A scratch directory, removed on scope exit.
class TempDir {
public:
    TempDir();
    ~TempDir();
    TempDir(const TempDir&) = delete;
    TempDir& operator=(const TempDir&) = delete;

    const std::string& path() const { return path_; }
    std::string file(const std::string& name) const;
    bool exists(const std::string& name) const;
    std::string read(const std::string& name) const;
    void write(const std::string& name, const std::string& body) const;
    std::vector<std::string> entries() const;

private:
    std::string path_;
};

std::string trim(const std::string& text);
std::string replace_all(std::string text, const std::string& from, const std::string& to);

}  // namespace e2e

#define E2E_CASE(name)                    \
    static void name();                   \
    namespace {                           \
    ::e2e::Registrar add_##name(#name, name); \
    }                                     \
    static void name()

#define CHECK(cond) ::e2e::check(static_cast<bool>(cond), __FILE__, __LINE__, #cond)
#define CHECK_EQ(got, want) ::e2e::check_eq((got), (want), __FILE__, __LINE__, #got)
#define CHECK_HAS(haystack, needle) \
    ::e2e::check_has((haystack), (needle), __FILE__, __LINE__, #haystack " holds " #needle)
