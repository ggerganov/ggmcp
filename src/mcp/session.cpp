#include "gg/mcp/session.hpp"

#include <chrono>
#include <cstdio>
#include <ctime>
#include <random>
#include <sstream>

namespace gg::mcp {
namespace {

std::uint64_t randomBits() {
    static std::mutex mutex;
    static std::mt19937_64 engine(std::random_device {}());
    const std::lock_guard<std::mutex> lock(mutex);
    return engine();
}

std::string hex(std::uint64_t value, std::size_t width) {
    std::ostringstream out;
    out.width(static_cast<std::streamsize>(width));
    out.fill('0');
    out << std::hex << value;
    return out.str();
}

} // namespace

std::string newSessionId(std::uint64_t counter) {
    return hex(randomBits(), 16) + hex(counter, 8) + hex(randomBits(), 8);
}

std::string iso8601(std::chrono::system_clock::time_point when) {
    const std::time_t wall = std::chrono::system_clock::to_time_t(when);
    std::tm utc {};
#if defined(_WIN32)
    gmtime_s(&utc, &wall);
#else
    gmtime_r(&wall, &utc);
#endif
    char stamp[32];
    std::snprintf(stamp, sizeof(stamp), "%04d-%02d-%02dT%02d:%02d:%02dZ", utc.tm_year + 1900,
                  utc.tm_mon + 1, utc.tm_mday, utc.tm_hour, utc.tm_min, utc.tm_sec);
    return stamp;
}

std::string SessionStore::create(Session session) {
    session.id = newSessionId(++counter_);
    session.bornAtMs = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch())
            .count());
    const std::string id = session.id;
    const std::lock_guard<std::mutex> lock(mutex_);
    sessions_.emplace(id, std::move(session));
    return id;
}

bool SessionStore::get(const std::string& id, Session& out) const {
    const std::lock_guard<std::mutex> lock(mutex_);
    auto it = sessions_.find(id);
    if (it == sessions_.end()) {
        return false;
    }
    out = it->second;
    return true;
}

bool SessionStore::update(const std::string& id, const std::function<void(Session&)>& change) {
    const std::lock_guard<std::mutex> lock(mutex_);
    auto it = sessions_.find(id);
    if (it == sessions_.end()) {
        return false;
    }
    change(it->second);
    return true;
}

bool SessionStore::remove(const std::string& id) {
    const std::lock_guard<std::mutex> lock(mutex_);
    return sessions_.erase(id) > 0;
}

std::size_t SessionStore::size() const {
    const std::lock_guard<std::mutex> lock(mutex_);
    return sessions_.size();
}

std::vector<std::string> SessionStore::ids() const {
    const std::lock_guard<std::mutex> lock(mutex_);
    std::vector<std::string> ids;
    ids.reserve(sessions_.size());
    for (const auto& entry : sessions_) {
        ids.push_back(entry.first);
    }
    return ids;
}

void SessionStore::clear() {
    const std::lock_guard<std::mutex> lock(mutex_);
    sessions_.clear();
}

gg::Json describeSession(const Session& session) {
    const auto now = std::chrono::steady_clock::now();
    const auto nowMs = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count());
    const std::uint64_t ageSeconds =
        nowMs > session.bornAtMs ? (nowMs - session.bornAtMs) / 1000 : 0;

    Json client = Json::object({{"name", session.clientName},
                               {"version", session.clientVersion}});

    Json session_ = Json::object({{"id", session.id},
                                  {"state", session.initialized ? "initialized" : "new"},
                                  {"protocolVersion", session.protocolVersion},
                                  {"transport", session.transport},
                                  {"origin", session.origin},
                                  {"remoteAddress", session.clientAddress},
                                  {"userAgent", session.userAgent},
                                  {"createdAt", iso8601(session.createdAt)},
                                  {"ageSeconds", static_cast<long long>(ageSeconds)},
                                  {"requests", static_cast<long long>(session.requests)}});
    if (!session.clientName.empty() || !session.clientVersion.empty()) {
        session_.set("client", client);
    }
    return session_;
}

gg::Json describeMissingSession(SessionPolicy policy, const std::string& transport) {
    return Json::object({{"id", Json()},
                         {"state", "stateless"},
                         {"policy", toString(policy)},
                         {"transport", transport}});
}

} // namespace gg::mcp
