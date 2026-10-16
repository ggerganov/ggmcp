#include "harness.hpp"

#include <dirent.h>
#include <unistd.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <utility>

namespace e2e {
namespace {

struct Entry {
    std::string name;
    void (*run)();
};

std::vector<Entry>& registry() {
    static std::vector<Entry> entries;
    return entries;
}

std::string shorten(const std::string& text) {
    constexpr std::size_t kMax = 400;
    std::string flat;
    for (char c : text) flat += (c == '\n' ? ' ' : c);
    if (flat.size() <= kMax) return flat;
    return flat.substr(0, kMax) + "...";
}

}  // namespace

Registrar::Registrar(const char* name, void (*run)()) {
    registry().push_back(Entry{name, run});
}

std::vector<std::string> names() {
    std::vector<std::string> out;
    for (const Entry& entry : registry()) out.push_back(entry.name);
    std::sort(out.begin(), out.end());
    return out;
}

void fail(const char* file, int line, const std::string& what) {
    throw Failure(std::string(file) + ":" + std::to_string(line) + ": " + what);
}

void check(bool ok, const char* file, int line, const std::string& what) {
    if (ok) return;
    fail(file, line, std::string("expected ") + what);
}

void check_eq_str(const std::string& got, const std::string& want, const char* file, int line,
              const std::string& what) {
    if (got == want) return;
    fail(file, line, std::string(what) + "\n    got:  " + shorten(got) + "\n    want: " + shorten(want));
}

void check_has(const std::string& haystack, const std::string& needle, const char* file, int line,
               const std::string& what) {
    if (haystack.find(needle) != std::string::npos) return;
    fail(file, line, std::string(what) + "\n    missing: " + shorten(needle) +
                         "\n    in:      " + shorten(haystack));
}

TempDir::TempDir() {
    std::string pattern =
        (std::filesystem::temp_directory_path() / "ggmcp-e2e-XXXXXX").string();
    std::vector<char> buffer(pattern.begin(), pattern.end());
    buffer.push_back('\0');
    const char* made = ::mkdtemp(buffer.data());
    if (made == nullptr) throw Failure("cannot make a scratch directory");
    std::error_code ec;
    path_ = std::filesystem::weakly_canonical(std::filesystem::path(buffer.data()), ec).string();
    if (path_.empty()) path_ = std::string(buffer.data());
}

TempDir::~TempDir() {
    std::error_code ec;
    std::filesystem::remove_all(path_, ec);
}

std::string TempDir::file(const std::string& name) const { return path_ + "/" + name; }

bool TempDir::exists(const std::string& name) const {
    std::error_code ec;
    return std::filesystem::exists(file(name), ec);
}

std::string TempDir::read(const std::string& name) const {
    std::ifstream in(file(name), std::ios::binary);
    if (!in) throw Failure("cannot read " + file(name));
    std::ostringstream body;
    body << in.rdbuf();
    return body.str();
}

void TempDir::write(const std::string& name, const std::string& body) const {
    std::ofstream out(file(name), std::ios::binary | std::ios::trunc);
    out.write(body.data(), static_cast<std::streamsize>(body.size()));
    out.close();
}

std::vector<std::string> TempDir::entries() const {
    std::vector<std::string> out;
    DIR* dir = ::opendir(path_.c_str());
    if (dir == nullptr) return out;
    while (const dirent* item = ::readdir(dir)) {
        const std::string name = item->d_name;
        if (name != "." && name != "..") out.push_back(name);
    }
    ::closedir(dir);
    std::sort(out.begin(), out.end());
    return out;
}

std::string trim(const std::string& text) {
    std::size_t begin = 0;
    while (begin < text.size() && std::isspace(static_cast<unsigned char>(text[begin]))) ++begin;
    std::size_t end = text.size();
    while (end > begin && std::isspace(static_cast<unsigned char>(text[end - 1]))) --end;
    return text.substr(begin, end - begin);
}

std::string replace_all(std::string text, const std::string& from, const std::string& to) {
    if (from.empty()) return text;
    std::string out;
    std::size_t at = 0;
    for (;;) {
        const std::size_t next = text.find(from, at);
        if (next == std::string::npos) {
            out += text.substr(at);
            return out;
        }
        out += text.substr(at, next - at);
        out += to;
        at = next + from.size();
    }
}

int run_all(const std::string& only) {
    std::vector<Entry> picked;
    for (const Entry& entry : registry()) {
        if (only.empty() || entry.name == only) picked.push_back(entry);
    }
    if (only.empty() == false && picked.empty()) {
        std::fprintf(stderr, "no such case: %s\n", only.c_str());
        return 2;
    }

    std::size_t failed = 0;
    for (const Entry& entry : picked) {
        std::string error;
        try {
            entry.run();
        } catch (const Failure& problem) {
            error = problem.what();
        } catch (const std::exception& problem) {
            error = std::string("unexpected exception: ") + problem.what();
        } catch (...) {
            error = "unexpected exception";
        }
        if (error.empty()) {
            std::printf("ok    %s\n", entry.name.c_str());
        } else {
            ++failed;
            std::printf("FAIL  %s\n%s\n", entry.name.c_str(), error.c_str());
        }
        std::fflush(stdout);
    }

    if (failed != 0) {
        std::printf("\n%zu of %zu case(s) failed\n", failed, picked.size());
        return 1;
    }
    std::printf("\nall %zu case(s) passed\n", picked.size());
    return 0;
}

}  // namespace e2e
