// Selection of the backends that are compiled in.
//
// This is the composition root of the JSON and HTTP abstractions: it is the one
// place that knows which concrete backends exist. Everything above it only sees
// gg::Json and the interfaces in gg/json_backend.hpp and gg/http.hpp.

#pragma once

#include <memory>
#include <string>
#include <vector>

#include "gg/http.hpp"
#include "gg/json_backend.hpp"

namespace gg::backends {

std::vector<std::string> jsonBackendNames();
std::vector<std::string> httpBackendNames();

// Install a named backend. Unknown names return false and fill `error`.
// "default" keeps whatever is installed right now.
bool selectJsonBackend(const std::string& name, std::string& error);
bool selectHttpBackend(const std::string& name, std::string& error);

// Install the default pair (nlohmann/json + cpp-httplib).
void useDefaults();

} // namespace gg::backends
