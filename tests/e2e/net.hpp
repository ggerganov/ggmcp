#pragma once

#include <string>
#include <vector>

#include "harness.hpp"

namespace e2e {

struct Header {
    std::string name;
    std::string value;
};

struct Message {
    int status = 0;
    std::string reason;
    std::vector<Header> headers;
    std::string body;
    std::string raw;

    bool has(const std::string& name) const;
    std::string header(const std::string& name) const;  // empty when absent
    std::size_t count(const std::string& name) const;
    Json json() const;  // throws when the body is not JSON
};

struct Call {
    std::string method = "GET";
    std::string path = "/";
    std::vector<Header> headers;
    std::string body;
    std::string content_type = "application/json";
    bool send_content_type = true;
    std::string accept = "application/json";  // empty sends no Accept at all
    std::string connection;                   // "close", "keep-alive" or nothing
    std::string chunk_body;                   // non-empty sends this chunked, as one chunk
    std::vector<std::string> chunks;          // non-empty sends these, one chunk each
    std::string chunk_size;                   // the size line to use, when chunking
    bool send_content_length = true;          // a POST with no length is a broken client
    std::string authority;                    // the Host header to send

    Call() = default;
    Call(const std::string& method_, const std::string& path_) : method(method_), path(path_) {}

    Call& with(const std::string& name, const std::string& value) {
        headers.push_back(Header{name, value});
        return *this;
    }
    std::string wire() const;  // the bytes to put on the wire
};

Call post(const std::string& path, const std::string& body);
Call get(const std::string& path);
Call del(const std::string& path);
Call verb(const std::string& method, const std::string& path);

// A socket held open, so that keep-alive and pipelining can be tested as they are
// meant to be: by reading a second response from the same connection.
class Connection {
public:
    Connection(const std::string& host, int port);
    ~Connection();
    Connection(const Connection&) = delete;
    Connection& operator=(const Connection&) = delete;

    void send(const Call&);        // write a request, read nothing
    Message response();            // read one response
    Message request(const Call&);  // both

    bool alive() const { return fd_ >= 0; }
    void close();

private:
    void feed();
    std::string need(std::size_t bytes);
    std::string line();

    int fd_ = -1;
    bool eof_ = false;
    std::string in_;
    std::string host_;
    int port_ = 0;
};

Message request(const std::string& host, int port, const Call&);

}  // namespace e2e
