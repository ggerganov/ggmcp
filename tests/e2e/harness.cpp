#include "harness.hpp"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <sstream>
#include <thread>

#include <arpa/inet.h>
#include <cstdint>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

namespace ggt {
namespace {

constexpr int kStartTimeoutSeconds = 20;
constexpr int kReadTimeoutSeconds = 20;

std::vector<std::string>& failures() {
    static std::vector<std::string>* instance = new std::vector<std::string>;
    return *instance;
}

std::string& binaryPath() {
    static std::string* instance = new std::string;
    return *instance;
}

std::vector<std::string>& tempRoots() {
    static std::vector<std::string>* instance = new std::vector<std::string>;
    return *instance;
}

std::string& logDirectory() {
    static std::string* instance = [] {
        std::string* directory = new std::string;
        std::vector<char> pattern = {'/', 't', 'm', 'p', '/', 'g', 'g', 'm', 'c', 'p', '-',
                                     'e', '2', 'e', '-', 'd', 'i', 'r', 'X', 'X', 'X', 'X', 0};
        const char* created = ::mkdtemp(pattern.data());
        *directory = created != nullptr ? created : "/tmp";
        return directory;
    }();
    return *instance;
}

std::string slurp(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    std::ostringstream buffer;
    buffer << in.rdbuf();
    return buffer.str();
}

void removeAllAtExit() {
    for (const auto& path : tempRoots()) {
        std::string command = "rm -rf '" + path + "'";
        if (std::system(command.c_str()) != 0) {
            continue;
        }
    }
}

// Returns the port the caller asked for, or -1 when the kernel should pick one.
int requestedPort(const std::vector<std::string>& args) {
    auto at = std::find(args.begin(), args.end(), "--port");
    if (at == args.end() || std::next(at) == args.end()) {
        return -1;
    }
    return std::atoi(std::next(at)->c_str());
}

bool connecting(int port) {
    const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        return false;
    }
    sockaddr_in address {};
    address.sin_family = AF_INET;
    address.sin_port = htons(static_cast<uint16_t>(port));
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    const bool up = ::connect(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0;
    ::close(fd);
    return up;
}

std::string readLog(const std::string& path) {
    return slurp(path);
}

void spawn(const std::vector<std::string>& args, const std::vector<std::string>& env,
           const std::string& logPath, int* stdinPipe, int* stdoutPipe, pid_t& pid,
           std::string& error) {
    std::vector<char*> argv;
    argv.reserve(args.size() + 1);
    for (const auto& arg : args) {
        argv.push_back(const_cast<char*>(arg.c_str()));
    }
    argv.push_back(nullptr);

    int inPipe[2] = {-1, -1};
    int outPipe[2] = {-1, -1};
    if (stdinPipe != nullptr && ::pipe(inPipe) != 0) {
        error = std::strerror(errno);
        return;
    }
    if (stdoutPipe != nullptr && ::pipe(outPipe) != 0) {
        error = std::strerror(errno);
        return;
    }

    const int log = ::open(logPath.c_str(), O_CREAT | O_TRUNC | O_WRONLY, 0600);
    if (log < 0) {
        error = std::string("cannot open the log: ") + std::strerror(errno);
        return;
    }

    const pid_t child = ::fork();
    if (child < 0) {
        ::close(log);
        error = std::strerror(errno);
        return;
    }
    if (child == 0) {
        if (stdinPipe != nullptr) {
            ::dup2(inPipe[0], STDIN_FILENO);
            ::close(inPipe[0]);
            ::close(inPipe[1]);
        }
        if (stdoutPipe != nullptr) {
            ::dup2(outPipe[1], STDOUT_FILENO);
            ::close(outPipe[0]);
            ::close(outPipe[1]);
        } else {
            ::dup2(log, STDOUT_FILENO);
        }
        ::dup2(log, STDERR_FILENO);
        ::close(log);
        if (stdinPipe == nullptr) {
            const int devNull = ::open("/dev/null", O_RDONLY);
            if (devNull >= 0) {
                ::dup2(devNull, STDIN_FILENO);
                ::close(devNull);
            }
        }
        for (const auto& entry : env) {
            const std::size_t equals = entry.find('=');
            if (equals == std::string::npos) {
                continue;
            }
            ::setenv(entry.substr(0, equals).c_str(), entry.substr(equals + 1).c_str(), 1);
        }
        ::execv(argv.front(), argv.data());
        std::fprintf(stderr, "exec %s failed: %s\n", argv.front(), std::strerror(errno));
        ::_exit(127);
    }

    ::close(log);
    if (stdinPipe != nullptr) {
        ::close(inPipe[0]);
        *stdinPipe = inPipe[1];
    }
    if (stdoutPipe != nullptr) {
        ::close(outPipe[1]);
        *stdoutPipe = outPipe[0];
    }
    pid = child;
    error.clear();
}

std::string nextLogPath(const std::string& tag) {
    static std::atomic<int> counter {0};
    return logDirectory() + "/" + tag + "-" + std::to_string(counter++) + ".log";
}

bool parseEndpoint(const std::string& log, int& port) {
    const std::string marker = "listening on http://";
    const std::size_t at = log.find(marker);
    if (at == std::string::npos) {
        return false;
    }
    const std::size_t colon = log.find(':', at + marker.size());
    if (colon == std::string::npos) {
        return false;
    }
    const std::size_t slash = log.find('/', colon);
    const std::string digits = log.substr(colon + 1, slash == std::string::npos
                                                           ? std::string::npos
                                                           : slash - colon - 1);
    for (const char c : digits) {
        if (std::isdigit(static_cast<unsigned char>(c)) == 0) {
            return false;
        }
    }
    port = std::atoi(digits.c_str());
    return port > 0;
}

} // namespace

void expect(const char* file, int line, const std::string& expression, bool ok,
            const std::string& detail) {
    if (ok) {
        return;
    }
    std::ostringstream out;
    out << file << ":" << line << ": " << expression;
    if (!detail.empty()) {
        out << " -- " << detail;
    }
    failures().push_back(out.str());
    std::cout << "    " << out.str() << "\n" << std::flush;
}

void note(const std::string& text) {
    std::cout << "    " << text << "\n" << std::flush;
}

void sleepFor(int millis) {
    std::this_thread::sleep_for(std::chrono::milliseconds(millis));
}

void setBinary(const std::string& path) {
    binaryPath() = path;
}

const std::string& binary() {
    return binaryPath();
}

std::string tempRoot(const std::string& tag) {
    std::vector<char> pattern;
    const std::string prefix = std::string(logDirectory()) + "/" + tag + "-XXXXXX";
    pattern.assign(prefix.begin(), prefix.end());
    pattern.push_back('\0');
    const char* created = ::mkdtemp(pattern.data());
    if (created == nullptr) {
        expect(__FILE__, __LINE__, "mkdtemp", false, std::strerror(errno));
        return "/tmp";
    }
    tempRoots().push_back(created);
    return created;
}

Process runBinary(const std::vector<std::string>& args, const std::vector<std::string>& env) {
    std::vector<std::string> argv;
    argv.push_back(binary());
    for (const auto& arg : args) {
        argv.push_back(arg);
    }

    Process process;
    std::string error;
    pid_t pid = -1;
    const std::string logPath = nextLogPath("run");
    spawn(argv, env, logPath, nullptr, nullptr, pid, error);
    if (!error.empty()) {
        process.output = error;
        return process;
    }

    int status = 0;
    while (::waitpid(pid, &status, 0) < 0 && errno == EINTR) {
        continue;
    }
    process.exitCode = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
    process.signaled = WIFSIGNALED(status);
    process.output = readLog(logPath);
    return process;
}

Server::Server(std::vector<std::string> args, std::vector<std::string> env)
    : logPath_(nextLogPath("server")) {
    std::vector<std::string> argv;
    argv.push_back(binary());
    const int requested = requestedPort(args);
    if (requested < 0) {
        args.push_back("--port");
        args.push_back("0");
    }
    for (const auto& arg : args) {
        argv.push_back(arg);
    }

    port_ = requested > 0 ? requested : 0;

    std::string error;
    spawn(argv, env, logPath_, nullptr, nullptr, pid_, error);
    if (!error.empty()) {
        expect(__FILE__, __LINE__, "the server process started", false, error);
        return;
    }
    started_ = true;
    const bool ready = requested > 0 ? waitForConnection(error) : waitForEndpoint(error);
    if (!ready) {
        expect(__FILE__, __LINE__, "the server reported its endpoint", false,
               error + " | log: " + readLog(logPath_));
        stop();
        started_ = false;
    }
}

Server::~Server() {
    stop();
}

bool Server::waitForEndpoint(std::string& error) {
    for (int attempt = 0; attempt < kStartTimeoutSeconds * 20; ++attempt) {
        const std::string log = readLog(logPath_);
        if (parseEndpoint(log, port_)) {
            return true;
        }
        int status = 0;
        const pid_t done = ::waitpid(pid_, &status, WNOHANG);
        if (done == pid_) {
            error = "the server exited before it was ready";
            pid_ = -1;
            return false;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    error = "the server did not report an endpoint in time";
    return false;
}

bool Server::waitForConnection(std::string& error) {
    for (int attempt = 0; attempt < kStartTimeoutSeconds * 20; ++attempt) {
        if (connecting(port_)) {
            return true;
        }
        if (::waitpid(pid_, nullptr, WNOHANG) == pid_) {
            pid_ = -1;
            error = "the server exited before it was ready";
            return false;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    error = "nothing answered on port " + std::to_string(port_);
    return false;
}

std::string Server::log() const {
    return readLog(logPath_);
}

bool Server::alive() const {
    if (!started_ || pid_ <= 0) {
        return false;
    }
    int status = 0;
    return ::waitpid(pid_, &status, WNOHANG) != pid_;
}

void Server::stop() {
    if (pid_ <= 0) {
        return;
    }
    ::kill(pid_, SIGTERM);
    int status = 0;
    for (int attempt = 0; attempt < 100; ++attempt) {
        if (::waitpid(pid_, &status, WNOHANG) == pid_) {
            pid_ = -1;
            return;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    ::kill(pid_, SIGKILL);
    ::waitpid(pid_, &status, 0);
    pid_ = -1;
}

StdioSession::StdioSession(std::vector<std::string> args, std::vector<std::string> env)
    : logPath_(nextLogPath("stdio")) {
    std::vector<std::string> argv;
    argv.push_back(binary());
    for (const auto& arg : args) {
        argv.push_back(arg);
    }

    std::string error;
    spawn(argv, env, logPath_, &in_, &out_, pid_, error);
    if (!error.empty()) {
        expect(__FILE__, __LINE__, "the stdio server started", false, error);
    }
}

StdioSession::~StdioSession() {
    if (in_ >= 0) {
        ::close(in_);
    }
    if (out_ >= 0) {
        ::close(out_);
    }
    if (pid_ > 0 && !reaped_) {
        ::kill(pid_, SIGKILL);
        int status = 0;
        ::waitpid(pid_, &status, 0);
    }
}

void StdioSession::send(const std::string& line) {
    const std::string payload = line + "\n";
    std::size_t sent = 0;
    while (sent < payload.size()) {
        const ssize_t written = ::write(in_, payload.data() + sent, payload.size() - sent);
        if (written <= 0) {
            expect(__FILE__, __LINE__, "the stdio server read the request", false,
                   std::strerror(errno));
            return;
        }
        sent += static_cast<std::size_t>(written);
    }
}

std::string StdioSession::receive() {
    std::string line;
    for (;;) {
        pollfd descriptor {out_, POLLIN, 0};
        const int ready = ::poll(&descriptor, 1, kReadTimeoutSeconds * 1000);
        if (ready <= 0) {
            expect(__FILE__, __LINE__, "the stdio server answered", false,
                   ready == 0 ? "timed out" : std::strerror(errno));
            return line;
        }
        char c = 0;
        const ssize_t read = ::read(out_, &c, 1);
        if (read <= 0) {
            return line;
        }
        if (c == '\n') {
            return line;
        }
        line.push_back(c);
    }
}

bool StdioSession::expectNoReply() {
    pollfd descriptor {out_, POLLIN, 0};
    return ::poll(&descriptor, 1, 500) <= 0;
}

void StdioSession::closeInput() {
    if (in_ >= 0) {
        ::close(in_);
        in_ = -1;
    }
}

int StdioSession::wait() {
    if (pid_ <= 0 || reaped_) {
        return 0;
    }
    int status = 0;
    while (::waitpid(pid_, &status, 0) < 0 && errno == EINTR) {
        continue;
    }
    reaped_ = true;
    status_ = status;
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

std::string StdioSession::log() const {
    return slurp(logPath_);
}

int runAll(const std::vector<std::pair<std::string, TestMain>>& tests, int argc, char** argv) {
    std::string filter;
    if (argc > 1) {
        filter = argv[1];
    }
    std::atexit(removeAllAtExit);

    int failed = 0;
    int selected = 0;
    for (const auto& test : tests) {
        if (!filter.empty() && test.first.find(filter) == std::string::npos) {
            continue;
        }
        ++selected;
        std::cout << "== " << test.first << "\n" << std::flush;
        failures().clear();
        test.second();
        if (failures().empty()) {
            std::cout << "  ok\n" << std::flush;
        } else {
            ++failed;
            std::cout << "  FAILED (" << failures().size() << ")\n" << std::flush;
        }
    }

    std::cout << "\n" << (selected - failed) << "/" << selected << " tests passed\n" << std::flush;
    return failed == 0 ? 0 : 1;
}

} // namespace ggt
