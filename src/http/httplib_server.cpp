// cpp-httplib HTTP backend.
//
// The only translation unit that includes an HTTP backend header. Everything
// above this level speaks gg::http::Request / gg::http::Response.

#include "gg/http.hpp"

#include <httplib.h>

#include <memory>
#include <string>
#include <utility>

namespace gg::http {
namespace {

constexpr std::size_t kMaxPayload = 64u * 1024u * 1024u;
constexpr int kKeepAliveSeconds = 20;

std::string upper(std::string value) {
    for (char& c : value) {
        c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    }
    return value;
}

class HttplibServer final : public IServer {
public:
    std::string name() const override { return "httplib"; }

    void setRouter(std::shared_ptr<const Router> router) override {
        router_ = std::move(router);
    }

    void setLogger(std::function<void(const std::string&)> logger) override {
        logger_ = std::move(logger);
    }

    bool bind(const std::string& host, int port, std::string& error) override {
        configure();
        if (port == 0) {
            port_ = server_.bind_to_any_port(host);
            if (port_ <= 0) {
                error = "failed to bind an ephemeral port on " + host;
                return false;
            }
            return true;
        }
        if (!server_.bind_to_port(host, port)) {
            error = "failed to bind " + host + ":" + std::to_string(port);
            return false;
        }
        port_ = port;
        return true;
    }

    bool listen(std::string& error) override {
        if (!server_.is_valid()) {
            error = "http backend is not usable";
            return false;
        }
        if (!server_.listen_after_bind()) {
            error = "http backend stopped listening";
            return false;
        }
        return true;
    }

    void stop() override { server_.stop(); }

    int port() const override { return port_; }

private:
    void configure() {
        server_.set_payload_max_length(kMaxPayload);
        server_.set_keep_alive_timeout(kKeepAliveSeconds);
        const auto handler = [this](const httplib::Request& request, httplib::Response& response) {
            handle(request, response);
        };
        server_.Get(kCatchAll, handler);
        server_.Post(kCatchAll, handler);
        server_.Put(kCatchAll, handler);
        server_.Patch(kCatchAll, handler);
        server_.Delete(kCatchAll, handler);
        server_.Options(kCatchAll, handler);
    }

    void handle(const httplib::Request& request, httplib::Response& response) {
        Request request_;
        request_.method = upper(request.method);
        request_.target = request.target;
        request_.path = request.path;
        request_.body = request.body;
        request_.remoteAddress = request.remote_addr;
        request_.remotePort = static_cast<int>(request.remote_port);
        for (const auto& header : request.headers) {
            request_.headers[header.first] = header.second;
        }
        for (const auto& param : request.params) {
            request_.query.emplace_back(param.first, param.second);
        }

        const Response routed = router_ ? router_->dispatch(request_)
                                        : Response::text(500, "no router installed");

        response.status = routed.status;
        for (const auto& header : routed.headers) {
            response.set_header(header.first, header.second);
        }
        if (!routed.body.empty()) {
            response.body = routed.body;
            if (routed.header("Content-Type").empty()) {
                response.set_header("Content-Type", "application/json");
            }
        }
        if (logger_) {
            std::string line = request_.method + " " + request_.target + " -> " +
                               std::to_string(routed.status);
            logger_(line);
        }
    }

    static constexpr const char* kCatchAll = ".*";

    httplib::Server server_;
    std::shared_ptr<const Router> router_;
    std::function<void(const std::string&)> logger_;
    int port_ = 0;
};

} // namespace

std::unique_ptr<IServer> makeHttplibServer() {
    return std::make_unique<HttplibServer>();
}

} // namespace gg::http
