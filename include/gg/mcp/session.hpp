// MCP session bookkeeping and the whoami view of a session.

#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <vector>

#include "gg/json.hpp"
#include "gg/mcp/config.hpp"

namespace gg::mcp {

struct Session {
    std::string id;
    std::string protocolVersion;
    std::string clientName;
    std::string clientVersion;
    std::string transport = "http";
    std::string origin;
    std::string clientAddress;
    std::string userAgent;
    std::chrono::system_clock::time_point createdAt = std::chrono::system_clock::now();
    std::uint64_t bornAtMs = 0;
    bool initialized = false;
    std::uint64_t requests = 0;
};

// Serialised store of the live sessions.
class SessionStore {
public:
    std::string create(Session session);
    bool get(const std::string& id, Session& out) const;
    bool update(const std::string& id, const std::function<void(Session&)>& change);
    bool remove(const std::string& id);
    std::size_t size() const;
    std::vector<std::string> ids() const;
    void clear();

private:
    mutable std::mutex mutex_;
    std::map<std::string, Session> sessions_;
    std::atomic<std::uint64_t> counter_ {0};
};

gg::Json describeSession(const Session& session);
gg::Json describeMissingSession(SessionPolicy policy, const std::string& transport);

std::string newSessionId(std::uint64_t counter);
std::string iso8601(std::chrono::system_clock::time_point when);

} // namespace gg::mcp
