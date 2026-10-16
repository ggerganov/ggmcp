// Bearer token authorization (RFC 6750 presentation, RFC 9728 metadata).

#pragma once

#include <chrono>
#include <mutex>
#include <string>

#include "gg/http.hpp"
#include "gg/json.hpp"
#include "gg/mcp/config.hpp"

namespace gg::mcp {

class Authorizer {
public:
    struct Decision {
        bool allowed = false;
        int status = 401;
        std::string reason;
        std::string wwwAuthenticate;
    };

    explicit Authorizer(Options options);

    // Checks the configured token source once, at start up. Returns false when
    // --require-token is set but no usable token can be read.
    bool prepare(std::string& error);

    bool enabled() const;
    std::string sourceDescription() const;

    Decision authorize(const http::Request& request) const;

    // RFC 9728 protected resource metadata for the resource this request targets.
    gg::Json resourceMetadata(const http::Request& request) const;
    std::string resourceMetadataText(const http::Request& request) const;

private:
    std::string currentToken(std::string& problem) const;

    Options options_;
    std::string startupToken_;
    bool hasStartupToken_ = false;

    mutable std::mutex mutex_;
    mutable std::string cachedToken_;
    mutable std::string cacheKey_;
    mutable std::chrono::steady_clock::time_point cacheAt_{};
};

} // namespace gg::mcp
