#pragma once

#include <mutex>
#include <string>

#include "ggmcp/json.hpp"

namespace ggmcp {

struct Config;

// A bearer-token resource server, off unless --require-token. The token is read
// once at start up and, when it comes from a file, read again whenever the file
// changes.
class TokenStore {
public:
    enum class Verdict { Ok, Missing, Wrong, InQuery };

    explicit TokenStore(const Config& config);

    // False means the configured source is unusable: fatal at start up.
    bool start(std::string& error);

    bool enabled() const { return enabled_; }
    std::string source() const;  // what the banner prints

    // `header` is the Authorization value; `query_has_token` says whether the query
    // string carries a token. The query is the problem, not the token: a token there
    // is a 401 even next to a correct Authorization header.
    Verdict check(const std::string& header, bool query_has_token) const;

    Json metadata(const std::string& origin, const std::string& endpoint) const;
    std::string challenge(const std::string& origin, const std::string& reason) const;

private:
    std::string current() const;

    bool enabled_ = false;
    std::string title_;
    std::string source_kind_;  // env | file | stdin
    std::string env_name_;
    std::string file_;
    mutable std::mutex mu_;
    mutable std::string cached_;
    mutable std::string file_stamp_;
};

bool constant_time_equal(const std::string& a, const std::string& b);

}  // namespace ggmcp
