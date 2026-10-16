#include "gg/http.hpp"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <sstream>

namespace gg::http {
namespace {

char lower(char c) {
    return static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
}

char upper(char c) {
    return static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
}

int hexDigit(char c) {
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    return -1;
}

std::string upper(std::string value) {
    for (char& c : value) {
        c = upper(c);
    }
    return value;
}

} // namespace

bool CaseInsensitiveLess::operator()(const std::string& lhs, const std::string& rhs) const {
    return std::lexicographical_compare(
        lhs.begin(), lhs.end(), rhs.begin(), rhs.end(),
        [](char lhsChar, char rhsChar) { return lower(lhsChar) < lower(rhsChar); });
}

std::string Request::header(const std::string& name, const std::string& fallback) const {
    auto it = headers.find(name);
    return it == headers.end() ? fallback : it->second;
}

bool Request::hasHeader(const std::string& name) const {
    return headers.find(name) != headers.end();
}

std::string Request::param(const std::string& name, const std::string& fallback) const {
    for (const auto& entry : query) {
        if (entry.first == name) {
            return entry.second;
        }
    }
    return fallback;
}

bool Request::hasParam(const std::string& name) const {
    return std::any_of(query.begin(), query.end(),
                       [&](const auto& entry) { return entry.first == name; });
}

std::string Request::scheme() const {
    return header("X-Forwarded-Proto", "http");
}

std::string Request::authority() const {
    std::string host = header("Host");
    if (!host.empty()) {
        return host;
    }
    return remoteAddress;
}

void Response::setHeader(const std::string& name, const std::string& value) {
    headers[name] = value;
}

void Response::setContentType(const std::string& value) {
    headers["Content-Type"] = value;
}

std::string Response::header(const std::string& name, const std::string& fallback) const {
    auto it = headers.find(name);
    return it == headers.end() ? fallback : it->second;
}

Response Response::text(int status, std::string body) {
    Response response;
    response.status = status;
    response.body = std::move(body);
    response.setContentType("text/plain; charset=utf-8");
    return response;
}

Response Response::json(int status, const gg::Json& body) {
    Response response;
    response.status = status;
    response.body = body.dump();
    response.setContentType("application/json");
    return response;
}

Response Response::empty(int status) {
    Response response;
    response.status = status;
    return response;
}

void Router::add(const std::string& method, const std::string& path, Handler handler) {
    routes_.push_back({upper(method), path, std::move(handler)});
}

bool Router::knowsPath(const std::string& path) const {
    return std::any_of(routes_.begin(), routes_.end(),
                       [&](const Route& route) { return route.path == path; });
}

std::vector<std::string> Router::methodsFor(const std::string& path) const {
    std::vector<std::string> methods;
    for (const auto& route : routes_) {
        if (route.path == path) {
            methods.push_back(route.method);
        }
    }
    return methods;
}

std::string Router::allowHeaderFor(const std::string& path) const {
    std::string allow;
    for (const auto& method : methodsFor(path)) {
        if (!allow.empty()) {
            allow += ", ";
        }
        allow += method;
    }
    return allow;
}

Response Router::errorBody(int status, const std::string& message) {
    gg::Json body = gg::Json::object({
        {"error", status},
        {"message", message},
    });
    Response response = Response::json(status, body);
    response.setHeader("Content-Type", "application/json");
    return response;
}

Response Router::dispatch(const Request& request) const {
    for (const auto& route : routes_) {
        if (route.path == request.path && route.method == upper(request.method)) {
            return route.handler(request);
        }
    }
    if (knowsPath(request.path)) {
        Response response = errorBody(405, "method not allowed");
        response.setHeader("Allow", allowHeaderFor(request.path));
        return response;
    }
    return errorBody(404, "not found");
}

std::string urlDecode(std::string_view value) {
    std::string out;
    out.reserve(value.size());
    for (std::size_t i = 0; i < value.size(); ++i) {
        if (value[i] == '+') {
            out.push_back(' ');
            continue;
        }
        if (value[i] != '%' || i + 2 >= value.size()) {
            out.push_back(value[i]);
            continue;
        }
        const int high = hexDigit(value[i + 1]);
        const int low = hexDigit(value[i + 2]);
        if (high < 0 || low < 0) {
            out.push_back(value[i]);
            continue;
        }
        out.push_back(static_cast<char>((high << 4) | low));
        i += 2;
    }
    return out;
}

std::string statusText(int status) {
    switch (status) {
    case 200: return "OK";
    case 202: return "Accepted";
    case 204: return "No Content";
    case 400: return "Bad Request";
    case 401: return "Unauthorized";
    case 403: return "Forbidden";
    case 404: return "Not Found";
    case 405: return "Method Not Allowed";
    case 406: return "Not Acceptable";
    case 415: return "Unsupported Media Type";
    case 500: return "Internal Server Error";
    case 501: return "Not Implemented";
    case 503: return "Service Unavailable";
    default: return "Unknown";
    }
}

} // namespace gg::http
