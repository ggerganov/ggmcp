#pragma once

#include <cstddef>
#include <string>
#include <vector>

namespace ggmcp {

enum class Transport { Http, Stdio };
enum class SessionPolicy { Lenient, Strict, Stateless };

struct Config {
    std::string host = "0.0.0.0";
    int port = 8081;
    Transport transport = Transport::Http;
    std::string endpoint = "/mcp";
    SessionPolicy session_policy = SessionPolicy::Lenient;

    std::string name = "ggmcp";
    std::string title;       // empty means: publish `name` as the title
    std::string instructions;
    std::string alias = "mcp";

    std::string fs_root;     // empty means: the home directory of the invoking user
    bool allow_outside_root = false;

    bool file_tools = true;
    bool bash_tool = true;

    bool require_token = false;
    std::string token_env;   // empty means: GGMCP_TOKEN
    std::string token_file;
    bool token_stdin = false;

    std::vector<std::string> allow_origin;
    bool strict_origin = false;
    bool cors = true;

    std::size_t max_body = 1024u * 1024u;
    bool quiet = false;

    const std::string& server_title() const { return title.empty() ? name : title; }
    bool confined() const { return !allow_outside_root; }
    std::string tool_name(const std::string& bare) const {
        return alias.empty() ? bare : alias + "-" + bare;
    }
    std::string url() const;
    std::string root_description() const;
};

enum class ParseResult { Ok, Help, Version, Error };

ParseResult parse_args(int argc, char** argv, Config& cfg, std::string& error);

const char* to_string(Transport transport);
const char* to_string(SessionPolicy policy);
bool parse_session_policy(const std::string& text, SessionPolicy& policy);
bool parse_transport(const std::string& text, Transport& transport);

std::string help_text();

}  // namespace ggmcp
