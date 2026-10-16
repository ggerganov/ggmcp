#include "ggmcp/paths.hpp"

#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <random>
#include <sstream>
#include <system_error>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include "ggmcp/config.hpp"
#include "ggmcp/json.hpp"

namespace ggmcp {
namespace fs = std::filesystem;

std::string home_dir() {
    if (const char* home = std::getenv("HOME")) {
        if (*home) return std::string(home);
    }
    return std::string();
}

PathPolicy PathPolicy::for_config(const Config& config) {
    PathPolicy policy;
    policy.confined = config.confined();
    if (!policy.confined) return policy;

    std::error_code ec;
    fs::path root = fs::weakly_canonical(fs::path(config.fs_root), ec);
    std::string text = ec ? config.fs_root : root.string();
    if (text.empty()) text = "/";
    while (text.size() > 1 && text.back() == '/') text.pop_back();
    policy.root = text;
    return policy;
}

bool PathPolicy::contains(const std::string& canonical) const {
    if (!confined) return true;
    if (root.empty()) return false;
    if (canonical == root) return true;
    std::string prefix = root;
    if (prefix.back() != '/') prefix += '/';
    return is_prefix(canonical, prefix);
}

bool PathPolicy::resolve(const std::string& requested, bool must_exist, std::string& out,
                         std::string& error) const {
    out.clear();
    if (requested.empty()) {
        error = "path must not be empty";
        return false;
    }
    if (requested.find('\0') != std::string::npos) {
        error = "path must not hold a NUL byte";
        return false;
    }

    const fs::path wanted(requested);
    fs::path candidate = wanted;
    if (confined && wanted.is_relative()) candidate = fs::path(root) / wanted;

    std::error_code ec;
    const fs::path canonical = fs::weakly_canonical(candidate, ec);
    if (ec) {
        error = "cannot resolve \"" + requested + "\": " + ec.message();
        return false;
    }
    const std::string text = canonical.string();

    if (confined && !contains(text)) {
        error = "\"" + requested + "\" is outside the root \"" + root + "\"";
        return false;
    }

    if (must_exist) {
        const fs::file_status status = fs::status(canonical, ec);
        if (ec) {
            error = "cannot open \"" + requested + "\": " + ec.message();
            return false;
        }
        if (fs::is_directory(status)) {
            error = "\"" + requested + "\" is a directory";
            return false;
        }
        if (!fs::is_regular_file(status)) {
            error = "\"" + requested + "\" is not a file";
            return false;
        }
    }

    out = text;
    return true;
}

bool read_file(const std::string& path, std::string& out, std::string& error) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        error = "cannot read \"" + path + "\"";
        return false;
    }
    std::ostringstream body;
    body << in.rdbuf();
    if (in.bad()) {
        error = "cannot read \"" + path + "\"";
        return false;
    }
    out = body.str();
    return true;
}

bool create_parent_dirs(const std::string& path, std::string& error) {
    const fs::path parent = fs::path(path).parent_path();
    if (parent.empty()) return true;

    std::error_code ec;
    if (fs::is_directory(parent, ec)) return true;

    fs::create_directories(parent, ec);
    const std::error_code written = ec;
    ec.clear();
    if (written || !fs::is_directory(parent, ec)) {
        error = "cannot create the directory \"" + parent.string() + "\"" +
                (written ? ": " + written.message() : std::string());
        return false;
    }
    return true;
}

namespace {

std::string random_suffix() {
    static std::mt19937_64 generator(std::random_device{}() ^
                                     static_cast<std::uint64_t>(
                                         std::chrono::steady_clock::now().time_since_epoch().count()));
    static std::mutex mu;
    std::lock_guard<std::mutex> lock(mu);
    std::uniform_int_distribution<std::uint64_t> dist;
    std::ostringstream out;
    out << std::hex << dist(generator);
    return out.str();
}

bool write_all(int fd, const std::string& data, std::string& error) {
    std::size_t offset = 0;
    while (offset < data.size()) {
        const ssize_t written = ::write(fd, data.data() + offset, data.size() - offset);
        if (written < 0) {
            if (errno == EINTR) continue;
            error = std::string("write failed: ") + std::strerror(errno);
            return false;
        }
        offset += static_cast<std::size_t>(written);
    }
    return true;
}

}  // namespace

bool write_file(const std::string& path, const std::string& data, std::string& error) {
    if (!create_parent_dirs(path, error)) return false;

    const fs::path target(path);
    std::error_code ec;
    if (fs::is_directory(target, ec)) {
        error = "\"" + path + "\" is a directory";
        return false;
    }

    const fs::path temp =
        target.parent_path() / ("ggmcp-tmp-" + target.filename().string() + "." +
                                std::to_string(static_cast<long>(::getpid())) + "." +
                                random_suffix());

    int fd = ::open(temp.c_str(), O_WRONLY | O_CREAT | O_EXCL, S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH);
    if (fd < 0) {
        error = "cannot write \"" + path + "\": " + std::strerror(errno);
        return false;
    }

    bool ok = write_all(fd, data, error);
    if (::fsync(fd) != 0) {
        error = std::string("write failed: ") + std::strerror(errno);
        ok = false;
    }
    ::close(fd);
    if (!ok) {
        ::unlink(temp.c_str());
        return false;
    }

    if (::rename(temp.c_str(), path.c_str()) != 0) {
        error = "cannot replace \"" + path + "\": " + std::strerror(errno);
        ::unlink(temp.c_str());
        return false;
    }
    return true;
}

bool append_file(const std::string& path, const std::string& data, std::string& error) {
    if (!create_parent_dirs(path, error)) return false;

    std::error_code ec;
    if (fs::is_directory(fs::path(path), ec)) {
        error = "\"" + path + "\" is a directory";
        return false;
    }

    const int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_APPEND,
                          S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH);
    if (fd < 0) {
        error = "cannot append to \"" + path + "\": " + std::strerror(errno);
        return false;
    }
    const bool ok = write_all(fd, data, error);
    ::close(fd);
    return ok;
}

}  // namespace ggmcp
