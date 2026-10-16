#pragma once

#include <memory>
#include <string>

namespace ggmcp {

struct Config;
class Core;
class TokenStore;
class ToolRegistry;

// The streamable HTTP backend, over the vendored HTTP library.
class HttpTransport {
public:
    HttpTransport(const Config& config, Core& core, const TokenStore& auth,
                  const ToolRegistry& tools);
    ~HttpTransport();

    HttpTransport(const HttpTransport&) = delete;
    HttpTransport& operator=(const HttpTransport&) = delete;

    // False once the listener is up; the reason is in `error`. A port that is
    // already serving is refused here rather than shared.
    bool bind(std::string& error);
    bool listening() const { return bound_; }

    // Blocks until `stop()`.
    void run();
    void stop();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    const Config* config_;
    Core* core_;
    const TokenStore* auth_;
    const ToolRegistry* tools_;
    bool bound_ = false;
};

}  // namespace ggmcp
