#include "proc.hpp"

#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <thread>

#include "harness.hpp"

#ifndef GGMCP_BINARY
#define GGMCP_BINARY "ggmcp"
#endif

namespace e2e {
namespace {

long long clock_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

std::string scratch(const std::string& kind) {
    std::string pattern = (std::filesystem::temp_directory_path() /
                           ("ggmcp-e2e-" + kind + "-XXXXXX"))
                              .string();
    std::vector<char> buffer(pattern.begin(), pattern.end());
    buffer.push_back('\0');
    const int fd = ::mkstemp(buffer.data());
    if (fd < 0) throw Failure("cannot make a scratch file");
    ::close(fd);
    return std::string(buffer.data());
}

int open_write(const std::string& path) {
    const int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, S_IRUSR | S_IWUSR);
    if (fd < 0) throw Failure("cannot open " + path);
    return fd;
}

}  // namespace

int free_port() {
    const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) throw Failure("cannot open a socket to pick a port");

    sockaddr_in address {};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = 0;
    if (::bind(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0) {
        ::close(fd);
        throw Failure(std::string("cannot bind a scratch port: ") + std::strerror(errno));
    }

    socklen_t size = sizeof(address);
    if (::getsockname(fd, reinterpret_cast<sockaddr*>(&address), &size) != 0) {
        ::close(fd);
        throw Failure("cannot read back a scratch port");
    }
    const int port = ntohs(address.sin_port);
    ::close(fd);
    return port;
}

Server::Server(int port, const std::vector<std::string>& args, bool stdio, bool wait_ready,
               bool own_arguments)
    : port_(port), stdio_(stdio) {
    const bool bare = own_arguments || stdio;
    out_path_ = scratch("out");
    err_path_ = scratch("err");

    std::vector<std::string> owned = args;
    if (!bare) {
        owned.push_back("--host");
        owned.push_back(host_);
        owned.push_back("--port");
        owned.push_back(std::to_string(port));
    }
    std::vector<std::string> words;
    words.push_back(GGMCP_BINARY);
    for (const std::string& word : owned) words.push_back(word);
    if (stdio_) {
        words.push_back("--transport");
        words.push_back("stdio");
    }

    std::vector<const char*> argv;
    for (const std::string& word : words) argv.push_back(word.c_str());
    argv.push_back(nullptr);

    int stdin_pipe[2] = {-1, -1};
    if (stdio_ && ::pipe(stdin_pipe) != 0) throw Failure("cannot make a pipe");

    const int out_fd = open_write(out_path_);
    const int err_fd = open_write(err_path_);

    const pid_t pid = ::fork();
    if (pid < 0) throw Failure("cannot start the server");

    if (pid == 0) {
        ::dup2(out_fd, STDOUT_FILENO);
        ::dup2(err_fd, STDERR_FILENO);
        if (stdio_) {
            ::dup2(stdin_pipe[0], STDIN_FILENO);
        } else {
            const int null = ::open("/dev/null", O_RDONLY);
            if (null >= 0) {
                ::dup2(null, STDIN_FILENO);
                ::close(null);
            }
        }
        ::close(out_fd);
        ::close(err_fd);
        if (stdio_) {
            ::close(stdin_pipe[0]);
            ::close(stdin_pipe[1]);
        }
        ::execv(argv.front(), const_cast<char* const*>(argv.data()));
        ::_exit(127);
    }

    ::close(out_fd);
    ::close(err_fd);
    if (stdio_) {
        ::close(stdin_pipe[0]);
        to_child_ = stdin_pipe[1];
    }
    pid_ = pid;
    live_ = true;

    if (!wait_ready) return;

    if (stdio_) {
        // Nothing to connect to: the banner on stderr is the ready signal, and a
        // quiet server prints nothing at all, so give it a moment and move on.
        const long long until = clock_ms() + 3000;
        while (stderr_log().empty() && clock_ms() < until) {
            int code = 0;
            if (exited(&code, 0)) {
                throw Failure("the stdio server exited with " + std::to_string(code) +
                              " before it printed anything:\n" + log());
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
        return;
    }

    const long long until = clock_ms() + 15000;
    for (;;) {
        int code = 0;
        int* code_ptr = &code;
        if (exited(code_ptr, 0)) {
            std::ostringstream why;
            why << "the server exited with " << code << " before it was ready:\n" << log();
            throw Failure(why.str());
        }
        try {
            Connection connection(host_, port_);
            connection.close();
            return;
        } catch (const Failure&) {
            // not listening yet
        }
        if (clock_ms() > until) throw Failure("the server never listened:\n" + log());
        std::this_thread::sleep_for(std::chrono::milliseconds(25));
    }
}

Server::~Server() { stop(); }

std::string Server::authority() const { return host_ + ":" + std::to_string(port_); }

std::string Server::url(const std::string& path) const { return "http://" + authority() + path; }

std::string Server::slurp(const std::string& path) const {
    std::ifstream in(path, std::ios::binary);
    if (!in) return std::string();
    std::ostringstream body;
    body << in.rdbuf();
    return body.str();
}

std::string Server::log() const { return slurp(out_path_) + slurp(err_path_); }

std::string Server::stderr_log() const { return slurp(err_path_); }

std::string Server::stdout_log() const { return slurp(out_path_); }

std::string Server::log_within(int timeout_ms) {
    const long long until = clock_ms() + timeout_ms;
    for (;;) {
        const std::string text = log();
        if (!text.empty()) return text;
        if (clock_ms() > until) return text;
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
}

bool Server::exited(int* code, int wait_ms) {
    if (pid_ < 0) {
        if (code != nullptr) *code = -1;
        return true;
    }
    const long long until = clock_ms() + wait_ms;
    for (;;) {
        int status = 0;
        const pid_t gone = ::waitpid(pid_, &status, WNOHANG);
        if (gone == pid_) {
            pid_ = -1;
            if (code != nullptr) {
                *code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
            }
            return true;
        }
        if (gone < 0 && errno != EINTR) {
            pid_ = -1;
            if (code != nullptr) *code = -1;
            return true;
        }
        if (clock_ms() >= until) return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
}

void Server::send_signal(int signal) const {
    if (!live_) return;
    if (::kill(pid_, signal) != 0) throw Failure(std::string("kill: ") + std::strerror(errno));
}

void Server::stop() {
    if (pid_ < 0) return;
    if (to_child_ >= 0) {
        ::close(to_child_);
        to_child_ = -1;
    }
    ::kill(pid_, SIGTERM);
    int code = 0;
    if (!exited(&code, 5000)) {
        ::kill(pid_, SIGKILL);
        exited(&code, 5000);
    }
}

void Server::close_stdin() {
    if (to_child_ >= 0) {
        ::close(to_child_);
        to_child_ = -1;
    }
}

void Server::send_line(const std::string& line) {
    if (to_child_ < 0) throw Failure("this server has no stdin");
    const std::string bytes = line + "\n";
    std::size_t sent = 0;
    while (sent < bytes.size()) {
        const ssize_t n = ::write(to_child_, bytes.data() + sent, bytes.size() - sent);
        if (n > 0) {
            sent += static_cast<std::size_t>(n);
            continue;
        }
        if (n < 0 && errno == EINTR) continue;
        throw Failure("cannot write to the server");
    }
}

std::string Server::read_line(int timeout_ms) {
    const long long until = clock_ms() + timeout_ms;
    for (;;) {
        const std::string all = slurp(out_path_);
        if (all.size() > out_at_) {
            const std::size_t end = all.find('\n', out_at_);
            if (end != std::string::npos) {
                std::string line = all.substr(out_at_, end - out_at_);
                out_at_ = end + 1;
                return line;
            }
        }
        if (clock_ms() > until) {
            throw Failure("the server printed nothing within " + std::to_string(timeout_ms) +
                          "ms; it said:\n" + log());
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
}

Message Server::request(const Call& call) {
    Call copy = call;
    if (copy.authority.empty()) copy.authority = authority();
    return e2e::request(host_, port_, copy);
}

Connection Server::connect() { return Connection(host_, port_); }

}  // namespace e2e
