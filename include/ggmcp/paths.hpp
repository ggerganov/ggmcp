#pragma once

#include <string>

namespace ggmcp {

struct Config;

// The boundary the file tools work inside. It is a boundary, not a sandbox:
// symlinks are resolved first and then tested for containment.
struct PathPolicy {
    std::string root;  // canonicalized, no trailing slash; empty when unconfined
    bool confined = true;

    static PathPolicy for_config(const Config& config);

    // Takes the requested path as given when unconfined. Otherwise a relative path
    // resolves against the root, an absolute one is used as sent, and the result is
    // refused when it lands outside the root. Errors quote the requested path.
    bool resolve(const std::string& requested, bool must_exist, std::string& out,
                 std::string& error) const;

    bool contains(const std::string& canonical) const;
};

bool read_file(const std::string& path, std::string& out, std::string& error);

// Create or replace. The bytes go to a sibling temporary that is renamed over the
// target, so a reader sees the old content or the new, and a write that cannot be
// completed leaves the target exactly as it was and leaves no temporary behind.
bool write_file(const std::string& path, const std::string& data, std::string& error);

bool append_file(const std::string& path, const std::string& data, std::string& error);

bool create_parent_dirs(const std::string& path, std::string& error);

std::string home_dir();

}  // namespace ggmcp
