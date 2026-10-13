// ggmcp - a tiny Model Context Protocol server built on vendored single-header libraries.
//
// SPDX-License-Identifier: MIT
//
// auth.hpp - bearer-token authorization for the HTTP transport.
//
// This is the *resource server* half of MCP's authorization model, which is the
// half a resource server can honestly implement alone:
//
//   * a request arrives with `Authorization: Bearer <token>`
//   * if it is missing or unusable, answer 401 with a `WWW-Authenticate`
//     header pointing at protected-resource metadata (RFC 9728)
//   * the metadata names the authorization servers that mint those tokens
//
// The other half - an authorization server that issues, expires and revokes
// tokens, with PKCE and dynamic client registration - is deliberately NOT here.
// ggmcp never mints a token and never talks to an authorization server; it only
// checks presented secrets against a list it was handed at startup. Point
// --authorization-server at a real one and the discovery document will say so,
// but nothing here enforces what that server says.
//
// Which leaves the caveat that matters: a static bearer token over plain HTTP is
// a postcard. It travels in clear text on every request, so this whole module is
// only worth as much as the transport under it. See the Security section of the
// README before turning this on outside loopback.

#ifndef GGGMCP_AUTH_HPP
#define GGGMCP_AUTH_HPP

#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

#include "ggmcp/http_server.hpp"

namespace ggmcp {

/// Compares two secrets without leaking their contents through timing.
///
/// The obvious `a == b` early-exits on the first differing byte, which turns a
/// wrong guess into a measurable signal and lets a caller recover a secret one
/// byte at a time. This walks both strings to the end instead.
bool constant_time_equals(const std::string& a, const std::string& b);

/// How the HTTP transport accepts credentials.
struct AuthOptions {
    /// Require a valid bearer token. Off by default, like the file and shell
    /// tools: a hello-world server should not silently change its wire
    /// behaviour, and nothing here is useful without TLS in front of it.
    bool required = false;

    /// Environment variables to read at startup. Defaults to GGMCP_TOKEN.
    std::vector<std::string> token_env;

    /// A credential handed over already safely outside argv - main.cpp fills this
    /// from stdin. Deliberately no --token FLAG: argv is world-readable through
    /// ps(1), copied into crash reports and kept in shell history, which is the
    /// whole thing a token is trying to avoid.
    std::string token_literal;

    /// Files to read at startup and re-read whenever they change, so a token can
    /// be rotated by rewriting the file instead of restarting the server. One
    /// token per line; blank lines and '#' comments are ignored.
    std::vector<std::string> token_files;

    /// Advertised in the WWW-Authenticate header.
    std::string realm = "ggmcp";

    /// Path of the protected-resource metadata document, per RFC 9728.
    std::string metadata_path = "/.well-known/oauth-protected-resource";

    /// Authorization servers to advertise. Names only - this server does not
    /// contact them.
    std::vector<std::string> authorization_servers;

    /// Absolute base URL to advertise in the metadata document, e.g.
    /// "https://mcp.example.com". Set this when a reverse proxy fronts the
    /// server; otherwise the request's Host header is used, falling back to the
    /// bound address.
    std::string public_url;
};

/// Who a request arrived as. Carries a label for logs and tools, never secret
/// material.
struct Identity {
    bool anonymous = true;
    std::string label;   ///< e.g. "env:GGMCP_TOKEN" or "file:tokens.txt:2"

    std::string describe() const { return anonymous ? "anonymous" : label; }
};

/// Outcome of authenticating one request.
struct AuthResult {
    bool ok = false;
    Identity identity;
    std::string oauth_error;   ///< "invalid_request" or "invalid_token", per RFC 6750
    std::string description;   ///< client-facing; never contains secret material

    static AuthResult allowed(Identity who) {
        AuthResult result;
        result.ok = true;
        result.identity = std::move(who);
        return result;
    }

    static AuthResult rejected(std::string code, std::string why) {
        AuthResult result;
        result.oauth_error = std::move(code);
        result.description = std::move(why);
        return result;
    }
};

/// The set of secrets this server will accept, and the check against them.
class TokenStore {
public:
    /// Reads every configured source. Returns false with a startup-facing
    /// message when authorization is required but no usable secret was found -
    /// starting an auth-required server with an empty list would lock everyone
    /// out, including the operator.
    static bool prepare(const AuthOptions& options, TokenStore& store, std::string& error);

    bool required() const { return required_; }
    std::size_t size() const;

    /// Path the metadata document is served from, and the base URL to advertise
    /// it under (empty means "derive it from the request").
    const std::string& metadata_path() const { return metadata_path_; }
    const std::string& public_url() const { return public_url_; }

    /// Decides whether `request` may proceed.
    ///
    /// Only the Authorization header is consulted. A token in the query string
    /// is refused even when the value is correct: the spec forbids it, and URLs
    /// end up in access logs, shell history and browser chrome.
    AuthResult authorize(const Request& request) const;

    /// The RFC 9728 protected-resource metadata document for this server.
    /// `resource` is the absolute endpoint URL as the client sees it.
    std::string metadata_json(const std::string& resource) const;

    /// The WWW-Authenticate value to accompany a 401.
    std::string www_authenticate(const std::string& metadata_url, const std::string& error,
                                const std::string& description) const;

    /// Labels of the configured sources, for the startup banner. Never includes
    /// secret material, so it is safe to print.
    std::vector<std::string> labels() const;

private:
    struct Secret {
        std::string label;   ///< where it came from, e.g. "env:GGMCP_TOKEN"
        std::string secret;
    };

    struct FileSource {
        std::string path;
        std::int64_t mtime = 0;
        std::uint64_t size = 0;
        std::vector<Secret> secrets;
    };

    /// Re-reads any file whose mtime or size changed since it was loaded.
    /// Callers must hold mutex_ - the server is thread-per-connection, so the
    /// token list is read from several threads at once.
    void refresh() const;

    static bool load_file(const std::string& path, std::vector<Secret>& out, std::string& error);

    /// Guards env_secrets_ and files_. Held for the whole check, not per entry,
    /// so a rotation cannot slip in halfway through a comparison.
    mutable std::mutex mutex_;

    std::vector<Secret> env_secrets_;
    mutable std::vector<FileSource> files_;
    std::vector<std::string> authorization_servers_;
    std::string realm_ = "ggmcp";
    std::string metadata_path_ = "/.well-known/oauth-protected-resource";
    std::string public_url_;
    bool required_ = false;
};

}  // namespace ggmcp

#endif  // GGGMCP_AUTH_HPP
