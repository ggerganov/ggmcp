#include "ggmcp/session.hpp"

#include <algorithm>
#include <chrono>
#include <random>

#include "ggmcp/protocol.hpp"

namespace ggmcp {
namespace {

std::string hex(const unsigned char* bytes, std::size_t n) {
    static const char* digits = "0123456789abcdef";
    std::string out;
    out.reserve(n * 2);
    for (std::size_t i = 0; i < n; ++i) {
        out += digits[(bytes[i] >> 4) & 0xf];
        out += digits[bytes[i] & 0xf];
    }
    return out;
}

}  // namespace

std::string new_session_id() {
    static std::mutex mu;
    static std::mt19937_64 generator(std::random_device{}());

    unsigned char bytes[20] = {};
    std::lock_guard<std::mutex> lock(mu);
    std::uniform_int_distribution<unsigned> byte(0, 255);
    for (std::size_t i = 0; i < 16; ++i) bytes[i] = static_cast<unsigned char>(byte(generator));
    const std::uint32_t salt = static_cast<std::uint32_t>(
        std::chrono::steady_clock::now().time_since_epoch().count());
    bytes[16] = static_cast<unsigned char>(salt & 0xff);
    bytes[17] = static_cast<unsigned char>((salt >> 8) & 0xff);
    bytes[18] = static_cast<unsigned char>((salt >> 16) & 0xff);
    bytes[19] = static_cast<unsigned char>((salt >> 24) & 0xff);
    return hex(bytes, sizeof(bytes));
}

std::string SessionStore::add(Session session) {
    if (session.id.empty()) session.id = new_session_id();
    const std::string id = session.id;
    std::lock_guard<std::mutex> lock(mu_);
    by_id_.emplace(id, std::move(session));
    return id;
}

bool SessionStore::get(const std::string& id, Session* out) const {
    std::lock_guard<std::mutex> lock(mu_);
    const auto it = by_id_.find(id);
    if (it == by_id_.end()) return false;
    if (out) *out = it->second;
    return true;
}

bool SessionStore::markInitialized(const std::string& id) {
    std::lock_guard<std::mutex> lock(mu_);
    const auto it = by_id_.find(id);
    if (it == by_id_.end()) return false;
    it->second.initialized = true;
    return true;
}

bool SessionStore::erase(const std::string& id) {
    std::lock_guard<std::mutex> lock(mu_);
    return by_id_.erase(id) > 0;
}

std::vector<Session> SessionStore::list() const {
    std::lock_guard<std::mutex> lock(mu_);
    std::vector<Session> out;
    out.reserve(by_id_.size());
    for (const auto& entry : by_id_) out.push_back(entry.second);
    std::sort(out.begin(), out.end(),
              [](const Session& a, const Session& b) { return a.id < b.id; });
    return out;
}

std::size_t SessionStore::size() const {
    std::lock_guard<std::mutex> lock(mu_);
    return by_id_.size();
}

}  // namespace ggmcp
