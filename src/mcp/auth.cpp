#include "gg/mcp/auth.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <sys/stat.h>

namespace gg::mcp {
namespace {

constexpr auto kFileRefresh = std::chrono::seconds(1);

std::string trim(const std::string& text) {
    const auto first = text.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) {
        return "";
    }
    const auto last = text.find_last_not_of(" \t\r\n");
    return text.substr(first, last - first + 1);
}

std::string quoted(const std::string& value) {
    std::string out = "\"";
    for (const char c : value) {
        if (c == '"' || c == '\\') {
            out.push_back('\\');
        }
        out.push_back(c);
    }
    out.push_back('"');
    return out;
}

bool readTokenFile(const std::string& path, std::string& token, std::string& problem) {
    std::error_code code;
    if (!std::filesystem::exists(path, code) || std::filesystem::is_directory(path, code)) {
        problem = "token file '" + path + "' is not readable";
        return false;
    }
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        problem = "cannot open token file '" + path + "'";
        return false;
    }
    std::ostringstream buffer;
    buffer << in.rdbuf();
    token = trim(buffer.str());
    if (token.empty()) {
        problem = "token file '" + path + "' is empty";
        return false;
    }
    return true;
}

std::string fileCacheKey(const std::string& path) {
    struct stat info;
    if (::stat(path.c_str(), &info) != 0) {
        return "missing";
    }
    std::ostringstream key;
    key << info.st_mtime << ':' << info.st_size << ':' << info.st_ino;
    return key.str();
}

bool sameToken(const std::string& left, const std::string& right) {
    if (left.size() != right.size()) {
        return false;
    }
    unsigned char difference = 0;
    for (std::size_t i = 0; i < left.size(); ++i) {
        difference = static_cast<unsigned char>(difference ^ (left[i] ^ right[i]));
    }
    return difference == 0;
}

bool startsWithBearer(const std::string& header, std::string& token) {
    constexpr const char* kBearer = "bearer";
    const std::size_t length = std::strlen(kBearer);
    if (header.size() <= length) {
        return false;
    }
    for (std::size_t i = 0; i < length; ++i) {
        if (std::tolower(static_cast<unsigned char>(header[i])) != kBearer[i]) {
            return false;
        }
    }
    char separator = header[length];
    if (separator != ' ' && separator != '\t') {
        return false;
    }
    std::size_t start = length + 1;
    while (start < header.size() && (header[start] == ' ' || header[start] == '\t')) {
        ++start;
    }
    token = trim(header.substr(start));
    return !token.empty();
}

} // namespace

Authorizer::Authorizer(Options options) : options_(std::move(options)) {}

bool Authorizer::prepare(std::string& error) {
    if (!options_.requireToken) {
        return true;
    }

    std::string token;
    if (options_.tokenStdin) {
        std::string line;
        if (!std::getline(std::cin, line)) {
            error = "--token-stdin needs a token on the first line of stdin";
            return false;
        }
        token = trim(line);
        if (token.empty()) {
            error = "--token-stdin read an empty token";
            return false;
        }
        startupToken_ = token;
    } else if (!options_.tokenFile.empty()) {
        std::string problem;
        if (!readTokenFile(options_.tokenFile, token, problem)) {
            error = problem;
            return false;
        }
        startupToken_ = token;
    } else {
        const char* fromEnv = std::getenv(options_.tokenEnv.c_str());
        if (fromEnv == nullptr || trim(fromEnv).empty()) {
            error = "no token to require: set $" + options_.tokenEnv +
                    ", or point --token-file at a token, or use --token-stdin";
            return false;
        }
        startupToken_ = trim(fromEnv);
    }

    hasStartupToken_ = true;
    return true;
}

bool Authorizer::enabled() const {
    return options_.requireToken;
}

std::string Authorizer::sourceDescription() const {
    if (!options_.requireToken) {
        return "off";
    }
    if (options_.tokenStdin) {
        return "stdin";
    }
    if (!options_.tokenFile.empty()) {
        return "file:" + options_.tokenFile;
    }
    return "env:" + options_.tokenEnv;
}

std::string Authorizer::currentToken(std::string& problem) const {
    if (options_.tokenStdin) {
        return startupToken_;
    }
    if (options_.tokenFile.empty()) {
        const char* fromEnv = std::getenv(options_.tokenEnv.c_str());
        if (fromEnv != nullptr && !trim(fromEnv).empty()) {
            return trim(fromEnv);
        }
        problem = "$" + options_.tokenEnv + " is not set";
        return "";
    }

    const std::string key = fileCacheKey(options_.tokenFile);
    const std::lock_guard<std::mutex> lock(mutex_);
    const auto now = std::chrono::steady_clock::now();
    const bool fresh = key == cacheKey_ && now - cacheAt_ < kFileRefresh;
    if (!fresh) {
        std::string token;
        if (!readTokenFile(options_.tokenFile, token, problem)) {
            return "";
        }
        cachedToken_ = token;
        cacheKey_ = key;
        cacheAt_ = now;
    }
    return cachedToken_;
}

Authorizer::Decision Authorizer::authorize(const http::Request& request) const {
    Decision decision;
    if (!enabled()) {
        decision.allowed = true;
        decision.status = 200;
        return decision;
    }

    const std::string metadata =
        request.scheme() + "://" + request.authority() + options_.metadataPath;
    const std::string challenge = "Bearer realm=" + quoted(options_.serverName);

    auto challengeWith = [&](const char* error, const std::string& description) {
        Decision fault;
        fault.allowed = false;
        fault.status = 401;
        fault.reason = description;
        fault.wwwAuthenticate = challenge + ", error=" + quoted(error) +
                                ", error_description=" + quoted(description) +
                                ", resource_metadata=" + quoted(metadata);
        return fault;
    };

    if (request.hasParam("token") || request.hasParam("access_token")) {
        return challengeWith("invalid_request",
                             "bearer tokens must not be sent in the query string");
    }

    const std::string header = request.header("Authorization");
    if (header.empty()) {
        return challengeWith("invalid_request", "the bearer token is missing");
    }

    std::string token;
    if (!startsWithBearer(header, token)) {
        return challengeWith("invalid_request", "the Authorization header must be 'Bearer <token>'");
    }

    std::string problem;
    const std::string expected = currentToken(problem);
    if (expected.empty()) {
        return challengeWith("server_error", "the server has no usable token: " + problem);
    }

    if (!sameToken(token, expected)) {
        return challengeWith("invalid_token", "the bearer token is not valid for this resource");
    }

    decision.allowed = true;
    decision.status = 200;
    return decision;
}

gg::Json Authorizer::resourceMetadata(const http::Request& request) const {
    const std::string base = request.scheme() + "://" + request.authority();
    Json servers = Json::array();
    servers.push_back(base);
    Json methods = Json::array();
    methods.push_back("header");
    Json scopes = Json::array();

    Json metadata = Json::object({{"resource", base + options_.endpoint},
                                  {"resource_name", options_.serverTitle},
                                  {"authorization_servers", servers},
                                  {"bearer_methods_supported", methods},
                                  {"scopes_supported", scopes},
                                  {"resource_documentation", base + options_.indexPath}});
    metadata.set("compliance_spec_version", "2025-06-18");
    return metadata;
}

std::string Authorizer::resourceMetadataText(const http::Request& request) const {
    return resourceMetadata(request).dump(2);
}

} // namespace gg::mcp
