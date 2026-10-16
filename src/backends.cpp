#include "gg/backends.hpp"

#include <memory>
#include <string>
#include <vector>

// Backends compiled into this build. Declared here to keep the backend specific
// factories out of the neutral interface headers.
namespace gg::json {
std::unique_ptr<IBackend> makeNlohmannBackend();
} // namespace gg::json

namespace gg::http {
std::unique_ptr<IServer> makeHttplibServer();
} // namespace gg::http

namespace gg::backends {
namespace {

using HttpServerFactory = std::unique_ptr<http::IServer> (*)();

HttpServerFactory& httpServerFactory() {
    static HttpServerFactory* instance = new HttpServerFactory(&http::makeHttplibServer);
    return *instance;
}

} // namespace

std::vector<std::string> jsonBackendNames() {
    return {"nlohmann"};
}

std::vector<std::string> httpBackendNames() {
    return {"httplib"};
}

bool selectJsonBackend(const std::string& name, std::string& error) {
    if (name == "nlohmann") {
        json::setBackend(json::makeNlohmannBackend());
        return true;
    }
    error = "unknown json backend '" + name + "', available: nlohmann";
    return false;
}

bool selectHttpBackend(const std::string& name, std::string& error) {
    if (name == "httplib") {
        httpServerFactory() = &http::makeHttplibServer;
        return true;
    }
    error = "unknown http backend '" + name + "', available: httplib";
    return false;
}

std::unique_ptr<http::IServer> makeHttpServer() {
    return httpServerFactory()();
}

void useDefaults() {
    std::string error;
    selectJsonBackend("nlohmann", error);
    selectHttpBackend("httplib", error);
}

} // namespace gg::backends
