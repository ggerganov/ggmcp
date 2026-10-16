#pragma once

#include <string>
#include <vector>

#include "net.hpp"

namespace e2e {

// A port nothing is listening on, handed out just before the server is started.
int free_port();

// The real binary, as a child process.
class Server {
public:
    // `own_arguments` means the argument list is exactly what the child gets.
    Server(int port, const std::vector<std::string>& args, bool stdio = false,
           bool wait_ready = true, bool own_arguments = false);
    ~Server();
    Server(const Server&) = delete;
    Server& operator=(const Server&) = delete;

    int port() const { return port_; }
    const std::string& host() const { return host_; }
    std::string url(const std::string& path = "/") const;
    std::string authority() const;

    // Everything the child has printed so far.
    std::string log() const;
    // The same, once the child has printed anything at all.
    std::string log_within(int timeout_ms);

    int pid() const { return pid_; }

    // The two streams apart: the stdio transport speaks the protocol on stdout.
    std::string stderr_log() const;
    std::string stdout_log() const;
    void close_stdin();

    // True once the child is gone; `code` then holds its status.
    bool exited(int* code, int wait_ms);

    // Send a signal the way a terminal or a supervisor would.
    void send_signal(int signal) const;

    void stop();

    Message request(const Call& call);
    Connection connect();

    // stdio transport only.
    void send_line(const std::string& line);
    std::string read_line(int timeout_ms);

private:
    std::string slurp(const std::string& path) const;

    int pid_ = -1;
    int port_ = 0;
    std::string host_ = "127.0.0.1";
    bool stdio_ = false;
    std::string out_path_;
    std::string err_path_;
    int to_child_ = -1;
    std::size_t out_at_ = 0;
    bool live_ = false;
};

}  // namespace e2e
