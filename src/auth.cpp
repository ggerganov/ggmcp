#include "ggmcp/auth.hpp"

#include <sys/stat.h>

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <sstream>

#include "ggmcp/config.hpp"

namespace ggmcp {
namespace {

std::string trim(const std::string& text) {
    std::size_t begin = 0;
    while (begin < text.size() &&
           std::isspace(static_cast<unsigned char>(text[begin])) != 0) {
        ++begin;
    }
    std::size_t end = text.size();
    while (end > begin && std::isspace(static_cast<unsigned char>(text[end - 1])) != 0) {
        --end;
    }
    return text.substr(begin, end - begin);
}

bool case_equal(const std::string& a, const std::string& b) {
    if (a.size() != b.size()) return false;
    for (std::size_t i = 0; i < a.size(); ++i) {
        if (std::tolower(static_cast<unsigned char>(a[i])) !=
            std::tolower(static_cast<unsigned char>(b[i]))) {
            return false;
        }
    }
    return true;
}

std::string stamp_of(const std::string& path) {
    struct stat st {};
    if (::stat(path.c_str(), &st) != 0) return std::string();
    return std::to_string(static_cast<long long>(st.st_mtime)) + "." +
           std::to_string(static_cast<long long>(st.st_size)) + "." +
           std::to_string(static_cast<long long>(st.st_ctime));
}

bool first_line_of(const std::string& path, std::string& token) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return false;
    std::string line;
    if (!std::getline(in, line)) return false;
    while (!line.empty() && line.back() == '\r') line.pop_back();
    token = trim(line);
    return !token.empty();
}

}  // namespace

bool constant_time_equal(const std::string& a, const std::string& b) {
    const std::size_t size = std::max(a.size(), b.size());
    unsigned char difference = static_cast<unsigned char>(a.size() ^ b.size());
    for (std::size_t i = 0; i < size; ++i) {
        const unsigned char x = i < a.size() ? static_cast<unsigned char>(a[i]) : 0;
        const unsigned char y = i < b.size() ? static_cast<unsigned char>(b[i]) : 0;
        difference = static_cast<unsigned char>(difference | static_cast<unsigned char>(x ^ y));
    }
    return difference == 0;
}

TokenStore::TokenStore(const Config& config) {
    enabled_ = config.require_token;
    title_ = config.server_title();
    if (config.token_stdin) {
        source_kind_ = "stdin";
    } else if (!config.token_file.empty()) {
        source_kind_ = "file";
        file_ = config.token_file;
    } else {
        source_kind_ = "env";
        env_name_ = config.token_env.empty() ? std::string("GGMCP_TOKEN") : config.token_env;
    }
}

bool TokenStore::start(std::string& error) {
    if (!enabled_) return true;

    std::string token;
    if (source_kind_ == "file") {
        if (!first_line_of(file_, token)) {
            error = "--token-file \"" + file_ + "\" holds no token";
            return false;
        }
    } else if (source_kind_ == "stdin") {
        std::string line;
        if (!std::getline(std::cin, line)) {
            error = "--token-stdin found nothing to read";
            return false;
        }
        while (!line.empty() && line.back() == '\r') line.pop_back();
        token = trim(line);
        if (token.empty()) {
            error = "--token-stdin found no token";
            return false;
        }
    } else {
        const char* value = std::getenv(env_name_.c_str());
        if (value == nullptr || trim(value).empty()) {
            error = "$" + env_name_ + " holds no token";
            return false;
        }
        token = trim(value);
    }

    std::lock_guard<std::mutex> lock(mu_);
    cached_ = token;
    if (source_kind_ == "file") file_stamp_ = stamp_of(file_);
    return true;
}

std::string TokenStore::source() const {
    if (!enabled_) return "off";
    std::lock_guard<std::mutex> lock(mu_);
    if (source_kind_ == "file") return "bearer token from \"" + file_ + "\", hot reloaded";
    if (source_kind_ == "stdin") return "bearer token from stdin";
    return "bearer token from $" + env_name_;
}

std::string TokenStore::current() const {
    std::lock_guard<std::mutex> lock(mu_);
    if (source_kind_ != "file") return cached_;

    const std::string stamp = stamp_of(file_);
    if (stamp.empty()) return std::string();
    if (stamp == file_stamp_) return cached_;

    std::string fresh;
    if (!first_line_of(file_, fresh)) return std::string();
    cached_ = fresh;
    file_stamp_ = stamp;
    return cached_;
}

TokenStore::Verdict TokenStore::check(const std::string& header, bool query_has_token) const {
    if (!enabled_) return Verdict::Ok;
    if (query_has_token) return Verdict::InQuery;

    const std::string want = current();
    const std::string got = trim(header);
    if (got.empty()) return Verdict::Missing;

    std::string presented;
    if (case_equal(got.substr(0, 6), "Bearer")) {
        presented = trim(got.substr(6));
    } else if (case_equal(got.substr(0, 5), "token")) {
        presented = trim(got.substr(5));
    } else {
        presented = got;
    }

    if (presented.empty()) return Verdict::Missing;
    if (want.empty()) return Verdict::Missing;
    return constant_time_equal(presented, want) ? Verdict::Ok : Verdict::Wrong;
}

Json TokenStore::metadata(const std::string& origin, const std::string& endpoint) const {
    Json document;
    document["bearer_methods_supported"] =
        Json::array({Json::object({{"header", "Authorization"}, {"prefix", "Bearer"}})});
    document["resource"] = origin + endpoint;
    document["resource_documentation"] = origin + "/";
    document["resource_name"] = title_;
    document["resource_policy_uri"] = origin + "/";
    document["scopes_supported"] = Json::array();
    return document;
}

std::string TokenStore::challenge(const std::string& origin, const std::string& reason) const {
    std::string challenge = "Bearer realm=\"ggmcp\", resource_metadata=\"" + origin +
                            "/.well-known/oauth-protected-resource\"";
    if (!reason.empty()) {
        challenge += ", error=\"invalid_token\", error_description=\"" + reason + "\"";
    }
    return challenge;
}

}  // namespace ggmcp
