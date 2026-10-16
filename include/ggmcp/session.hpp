#pragma once

#include <cstdint>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace ggmcp {

struct Session {
    std::string id;
    std::string protocol_version;
    std::string client_name;
    std::string client_version;
    std::string transport = "http";
    std::string address;
    bool initialized = false;
};

// `initialize` adds a session, `notifications/initialized` marks it usable and
// `DELETE` takes it away again. Nothing is ever expired or capped.
class SessionStore {
public:
    std::string add(Session session);
    bool get(const std::string& id, Session* out) const;
    bool markInitialized(const std::string& id);
    bool erase(const std::string& id);
    std::vector<Session> list() const;
    std::size_t size() const;

private:
    mutable std::mutex mu_;
    std::unordered_map<std::string, Session> by_id_;
};

std::string new_session_id();

}  // namespace ggmcp
