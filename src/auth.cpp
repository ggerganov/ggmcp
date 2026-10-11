// ggmcp - a tiny Model Context Protocol server in dependency-free C++17.
//
// SPDX-License-Identifier: MIT
//
// auth.cpp - bearer-token checks. See auth.hpp for what this does and does not
// do about authorization; the short version is that it validates secrets it was
// handed and advertises protected-resource metadata, and issues no tokens.

#include "ggmcp/auth.hpp"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <utility>

#include "ggmcp/json.hpp"
#include "ggmcp/log.hpp"

namespace ggmcp {
namespace {

namespace fs = std::filesystem;

std::string lowered(std::string text) {
    for (char& c : text) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return text;
}

bool is_space(char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\v' || c == '\f'; }

std::string trimmed(const std::string& text) {
    const std::size_t first = text.find_first_not_of(" \t\r\n\v\f");
    if (first == std::string::npos) return {};
    const std::size_t last = text.find_last_not_of(" \t\r\n\v\f");
    return text.substr(first, last - first + 1);
}

/// True when the query string looks like it is carrying a credential.
///
/// Deliberately broader than the two names RFC 6750 defines: clients have been
/// observed trying ?token=, ?apiKey= and ?key=, and every one of those puts the
/// secret in access logs, proxy logs, shell history and browser chrome.
bool query_carries_credential(const std::string& query) {
    static const char* const kNames[] = {"access_token", "token",      "bearer",     "bearer_token",
                                         "id_token",     "api_key",    "apikey",     "x-api-key",
                                         "authorization", "key",       "secret",     "client_secret"};

    std::size_t position = 0;
    while (position < query.size()) {
        const std::size_t ampersand = query.find('&', position);
        const std::string part = query.substr(position, ampersand == std::string::npos ? std::string::npos
                                                                                       : ampersand - position);
        const std::size_t equals = part.find('=');
        const std::string name = lowered(trimmed(equals == std::string::npos ? part : part.substr(0, equals)));
        for (const char* name_to_reject : kNames) {
            if (name == name_to_reject) return true;
        }
        if (ampersand == std::string::npos) break;
        position = ampersand + 1;
    }
    return false;
}

/// Pulls the credential out of an `Authorization: Bearer ...` value.
bool bearer_from_header(const std::string& header, std::string& token, std::string& reason) {
    if (header.empty()) {
        reason = "no Authorization header was sent";
        return false;
    }
    if (header.size() < 7 || lowered(header.substr(0, 6)) != "bearer" || !is_space(header[6])) {
        reason = "this server accepts the Bearer scheme only";
        return false;
    }
    token = trimmed(header.substr(7));
    if (token.empty()) {
        reason = "the bearer token is empty";
        return false;
    }
    return true;
}

bool file_stamp(const std::string& path, std::int64_t& mtime, std::uint64_t& size) {
    std::error_code ec;
    const auto time = fs::last_write_time(path, ec);
    if (ec) return false;
    mtime = static_cast<std::int64_t>(time.time_since_epoch().count());
    const std::uintmax_t bytes = fs::file_size(path, ec);
    size = ec ? 0 : static_cast<std::uint64_t>(bytes);
    return true;
}

/// A header value that could be mistaken for a realm or a URL must not be able
/// to break out of the quoted string it is interpolated into.
std::string escape_quoted(const std::string& text) {
    std::string out;
    out.reserve(text.size());
    for (const char c : text) {
        if (c == '"' || c == '\\') out.push_back('\\');
        if (!is_space(c) || c == ' ') out.push_back(c);
    }
    return out;
}

void warn_if_weak(const std::string& label, const std::string& secret) {
    if (secret.size() >= 16) return;
    log_line("warning: " + label + " holds a " + std::to_string(secret.size()) +
             "-character token; anything that can reach this port can try guesses");
}

}  // namespace

bool constant_time_equals(const std::string& a, const std::string& b) {
    // The length is not treated as secret: without padding it is observable no
    // matter how the comparison is written, and knowing a token's length does
    // not narrow the search in any useful way. Every byte is still examined so
    // that the *contents* leak nothing.
    if (a.size() != b.size()) return false;
    unsigned char difference = 0;
    for (std::size_t i = 0; i < a.size(); ++i) {
        difference |= static_cast<unsigned char>(a[i] ^ b[i]);
    }
    return difference == 0;
}

bool TokenStore::load_file(const std::string& path, std::vector<Secret>& out, std::string& error) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        error = "the token file \"" + path + "\" could not be opened";
        return false;
    }

    std::vector<Secret> secrets;
    std::string line;
    std::size_t number = 0;
    while (std::getline(in, line)) {
        ++number;
        if (!line.empty() && line.back() == '\r') line.pop_back();  // CRLF-edited files
        const std::string secret = trimmed(line);
        if (secret.empty() || secret[0] == '#') continue;
        secrets.push_back({path + ":" + std::to_string(number), secret});
    }

    if (secrets.empty()) {
        error = "the token file \"" + path + "\" contains no tokens";
        return false;
    }
    out = std::move(secrets);
    return true;
}

bool TokenStore::prepare(const AuthOptions& options, TokenStore& store, std::string& error) {
    store.env_secrets_.clear();
    store.files_.clear();
    store.authorization_servers_ = options.authorization_servers;
    store.realm_ = options.realm.empty() ? std::string("ggmcp") : options.realm;
    store.metadata_path_ = options.metadata_path.empty() ? std::string("/.well-known/oauth-protected-resource")
                                                        : options.metadata_path;
    store.public_url_ = options.public_url;
    store.required_ = options.required;
    while (store.public_url_.size() > 1 && store.public_url_.back() == '/') store.public_url_.pop_back();

    if (!options.required) return true;

    // $GGMCP_TOKEN is the default source, but only when no source was named at
    // all: an operator who configured --token-file should not also be made to set
    // an environment variable they never asked for.
    const bool named_a_source =
        !options.token_env.empty() || !options.token_files.empty() || !options.token_literal.empty();

    std::vector<std::string> names = options.token_env;
    if (!named_a_source) names.emplace_back("GGMCP_TOKEN");

    if (!options.token_literal.empty()) {
        const std::string secret = trimmed(options.token_literal);
        if (secret.empty()) {
            error = "a token was handed over empty";
            return false;
        }
        store.env_secrets_.push_back({"stdin", secret});
    }

    for (const std::string& name : names) {
        const char* raw = std::getenv(name.c_str());
        const std::string secret = raw ? trimmed(raw) : std::string();
        if (secret.empty()) {
            // Failing here is the point. Booting an auth-required server with an
            // empty list would accept nothing, and the operator finds that out by
            // being locked out.
            error = "authorization is required, but the environment variable \"" + name +
                    "\" is unset or empty";
            return false;
        }
        store.env_secrets_.push_back({"env:" + name, secret});
    }

    for (const std::string& path : options.token_files) {
        FileSource source;
        source.path = path;
        if (!load_file(path, source.secrets, error)) return false;
        file_stamp(path, source.mtime, source.size);
        store.files_.push_back(std::move(source));
    }

    for (const Secret& secret : store.env_secrets_) warn_if_weak(secret.label, secret.secret);
    for (const FileSource& source : store.files_) {
        for (const Secret& secret : source.secrets) warn_if_weak(secret.label, secret.secret);
    }
    return true;
}

std::size_t TokenStore::size() const {
    std::lock_guard<std::mutex> lock(mutex_);
    refresh();
    std::size_t total = env_secrets_.size();
    for (const FileSource& source : files_) total += source.secrets.size();
    return total;
}

std::vector<std::string> TokenStore::labels() const {
    std::lock_guard<std::mutex> lock(mutex_);
    refresh();
    std::vector<std::string> out;
    for (const Secret& secret : env_secrets_) out.push_back(secret.label);
    for (const FileSource& source : files_) {
        for (const Secret& secret : source.secrets) out.push_back(secret.label);
    }
    return out;
}

void TokenStore::refresh() const {
    // Precondition: mutex_ is held (see size(), labels(), authorize()).
    for (FileSource& source : files_) {
        std::int64_t mtime = 0;
        std::uint64_t size = 0;
        // A file that vanished or went unreadable mid-request keeps the tokens it
        // had. Rotations are usually a rename onto the path, so a half-written or
        // momentarily missing file is normal and must not lock everyone out.
        if (!file_stamp(source.path, mtime, size)) continue;
        if (mtime == source.mtime && size == source.size) continue;

        std::vector<Secret> reloaded;
        std::string error;
        if (load_file(source.path, reloaded, error)) {
            log_line("reloaded " + source.path + " (" + std::to_string(reloaded.size()) + " token" +
                     (reloaded.size() == 1 ? "" : "s") + ")");
            source.secrets = std::move(reloaded);
        } else {
            log_line("keeping the previous tokens from " + source.path + ": " + error);
        }
        // Stamped either way, so a broken file is not re-read on every request.
        source.mtime = mtime;
        source.size = size;
    }
}

AuthResult TokenStore::authorize(const Request& request) const {
    if (!required_) return AuthResult::allowed(Identity{});

    std::lock_guard<std::mutex> lock(mutex_);
    refresh();

    if (query_carries_credential(request.query)) {
        return AuthResult::rejected("invalid_request",
                                    "credentials must be sent in the Authorization header, not the "
                                    "query string");
    }

    std::string presented;
    std::string reason;
    if (!bearer_from_header(request.header("authorization"), presented, reason)) {
        // A missing credential is invalid_request; a present-but-wrong one is
        // invalid_token. RFC 6750 distinguishes them and clients branch on it.
        const bool absent = reason == "no Authorization header was sent";
        return AuthResult::rejected(absent ? "invalid_request" : "invalid_token", reason);
    }

    bool matched = false;
    std::string label;
    // Walks every secret even after a match, so the time taken does not reveal
    // which entry in the list a caller happened to guess.
    for (const Secret& secret : env_secrets_) {
        if (constant_time_equals(presented, secret.secret)) {
            matched = true;
            label = secret.label;
        }
    }
    for (const FileSource& source : files_) {
        for (const Secret& secret : source.secrets) {
            if (constant_time_equals(presented, secret.secret)) {
                matched = true;
                label = secret.label;
            }
        }
    }

    if (!matched) {
        // Never "token not found" or "token expired" with any precision, and never
        // a prefix of what was sent: this string goes to the caller.
        return AuthResult::rejected("invalid_token", "the bearer token is not recognized");
    }

    Identity identity;
    identity.anonymous = false;
    identity.label = std::move(label);
    return AuthResult::allowed(std::move(identity));
}

std::string TokenStore::metadata_json(const std::string& resource) const {
    Object document;
    document["resource"] = Value::make_string(resource);

    Array bearer_methods;
    bearer_methods.push_back(Value::make_string("header"));
    document["bearer_methods_supported"] = Value::make_array(std::move(bearer_methods));

    Array servers;
    for (const std::string& url : authorization_servers_) servers.push_back(Value::make_string(url));
    // Absent rather than empty on purpose. ggmcp is not an authorization server:
    // it mints nothing, so there is nothing to advertise unless the operator
    // named a real one. A client that insists on full OAuth discovery will not
    // get anywhere here, which is the honest answer.
    if (!servers.empty()) document["authorization_servers"] = Value::make_array(std::move(servers));

    // No scopes_supported: this server does not enforce scopes, and advertising
    // ones it ignores would be a claim the code does not keep.
    return Value::make_object(std::move(document)).dump();
}

std::string TokenStore::www_authenticate(const std::string& metadata_url, const std::string& error,
                                       const std::string& description) const {
    std::ostringstream out;
    out << "Bearer realm=\"" << escape_quoted(realm_) << '"'
        << ", error=\"" << escape_quoted(error) << '"'
        << ", error_description=\"" << escape_quoted(description) << '"'
        << ", resource_metadata=\"" << escape_quoted(metadata_url) << '"';
    return out.str();
}

}  // namespace ggmcp
