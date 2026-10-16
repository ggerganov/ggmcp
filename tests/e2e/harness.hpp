// Tiny test harness used by the end to end tests: the server is always started
// as a real process and driven over a real socket or through stdio.

#pragma once

#include <string>
#include <utility>
#include <vector>

#include <sys/types.h>

#include "http_client.hpp"

namespace ggt {

void expect(const char* file, int line, const std::string& expression, bool ok,
            const std::string& detail);
void note(const std::string& text);
void sleepFor(int millis);

using TestMain = void (*)();

int runAll(const std::vector<std::pair<std::string, TestMain>>& tests, int argc, char** argv);

void setBinary(const std::string& path);
const std::string& binary();

// A fresh, empty directory under the system temp directory, removed at exit.
std::string tempRoot(const std::string& tag);

// A process that has already finished.
struct Process {
    int exitCode = -1;
    bool signaled = false;
    std::string output; // stdout and stderr, merged
};

Process runBinary(const std::vector<std::string>& args, const std::vector<std::string>& env = {});

// A running ggmcp over streamable HTTP. The port is picked by the kernel and
// read back from the server's own start up banner.
class Server {
public:
    explicit Server(std::vector<std::string> args, std::vector<std::string> env = {});
    ~Server();

    Server(const Server&) = delete;
    Server& operator=(const Server&) = delete;

    int port() const { return port_; }
    std::string log() const;
    bool alive() const;
    void stop();

private:
    bool waitForEndpoint(std::string& error);
    bool waitForConnection(std::string& error);

    pid_t pid_ = -1;
    int port_ = 0;
    std::string logPath_;
    bool started_ = false;
};

// A running ggmcp over stdio.
class StdioSession {
public:
    explicit StdioSession(std::vector<std::string> args, std::vector<std::string> env = {});
    ~StdioSession();

    StdioSession(const StdioSession&) = delete;
    StdioSession& operator=(const StdioSession&) = delete;

    void send(const std::string& line);
    std::string receive();     // one reply line, empty on timeout or end of stream
    bool expectNoReply();      // true when the server stays quiet for a moment
    void closeInput();
    int wait();
    std::string log() const;

private:
    int in_ = -1;
    int out_ = -1;
    pid_t pid_ = -1;
    bool reaped_ = false;
    int status_ = 0;
    std::string logPath_;
};

} // namespace ggt

#define EXPECT(condition, detail) \
    ::ggt::expect(__FILE__, __LINE__, #condition, static_cast<bool>(condition), (detail))
