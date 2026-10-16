#pragma once

#include <cstdio>
#include <mutex>
#include <string>

namespace ggmcp {

// Progress goes to stderr when the stdio transport owns stdout, to stdout otherwise.
class Log {
public:
    explicit Log(bool quiet = false, bool stderr_only = false)
        : quiet_(quiet), stderr_only_(stderr_only) {}

    void set_quiet(bool quiet) { quiet_ = quiet; }

    void line(const std::string& text) const {
        if (quiet_) return;
        std::lock_guard<std::mutex> lock(mu_);
        std::FILE* out = stderr_only_ ? stderr : stdout;
        std::fputs(text.c_str(), out);
        std::fputs("\n", out);
        std::fflush(out);
    }

    void warn(const std::string& text) const {
        std::lock_guard<std::mutex> lock(mu_);
        std::fputs(text.c_str(), stderr);
        std::fputs("\n", stderr);
        std::fflush(stderr);
    }

private:
    bool quiet_;
    bool stderr_only_;
    mutable std::mutex mu_;
};

}  // namespace ggmcp
