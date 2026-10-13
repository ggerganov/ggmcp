// ggmcp - a tiny Model Context Protocol server built on vendored single-header libraries.
//
// filesystem_tools.cpp - read / write / edit / bash.
//
// The tool names mirror the ones common agent harnesses ship, but the security
// posture is the opposite of a harness: there is no human watching this
// process. So file access is confined to a canonicalised root, and the shell
// is its own switch: --no-bash takes it away, and the file tools can be had
// without it.

#include "ggmcp/tools.hpp"

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include <csignal>
#include <ctime>
#include <fcntl.h>
#include <poll.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "ggmcp/log.hpp"

namespace ggmcp {

namespace fs = std::filesystem;

Value tool_annotations(const std::string& title, bool read_only, bool destructive, bool idempotent,
                       bool open_world) {
    Object object;
    object["title"] = Value(title);
    object["readOnlyHint"] = Value(read_only);
    object["destructiveHint"] = Value(destructive);
    object["idempotentHint"] = Value(idempotent);
    object["openWorldHint"] = Value(open_world);
    return Value(std::move(object));
}

namespace {

/// `fs::canonical` that tolerates path components which do not exist yet: walks
/// up to the nearest existing ancestor, canonicalises that, and re-attaches the
/// remaining components verbatim. `write` needs it so a client can create
/// `deep/deeper/file.txt` in one call, and normalising first means the
/// re-attached tail can never contain a "..".
fs::path canonicalise(const fs::path& in, std::error_code& ec) {
    fs::path canonical = fs::canonical(in, ec);
    if (!ec) return canonical;

    fs::path probe = in.lexically_normal();
    std::vector<std::string> missing;
    while (true) {
        missing.emplace_back(probe.filename().string());
        probe = probe.parent_path();

        ec.clear();
        const fs::path base = fs::canonical(probe, ec);
        if (!ec) {
            fs::path out = base;
            for (auto it = missing.rbegin(); it != missing.rend(); ++it) out /= *it;
            return out;
        }

        if (probe == probe.parent_path()) return canonical;  // hit the filesystem root
    }
}

}  // namespace

bool confine_path(const std::string& root, const std::string& path, bool allow_outside,
                  std::string& resolved, std::string& error) {
    resolved.clear();
    if (path.empty()) {
        error = "\"path\" must not be empty";
        return false;
    }

    std::error_code ec;
    const fs::path raw(path);
    const fs::path candidate = raw.is_absolute() ? raw : fs::path(root) / raw;

    // Resolve symlinks and ".." *before* testing containment - a lexical check
    // on the raw string is exactly what lets "../../etc/passwd" or a link that
    // points out of the tree through.
    const fs::path canonical = canonicalise(candidate, ec);
    if (ec) {
        error = "cannot resolve \"" + candidate.string() + "\": " + ec.message();
        return false;
    }

    if (!allow_outside) {
        const fs::path canonical_root = fs::canonical(root, ec);
        if (ec) {
            error = "the workspace root \"" + root + "\" cannot be resolved: " + ec.message();
            return false;
        }
        const std::string want = canonical_root.string();
        const std::string have = canonical.string();
        const bool inside = have == want || (have.size() > want.size() &&
                                            have.compare(0, want.size(), want) == 0 &&
                                            have[want.size()] == '/');
        if (!inside) {
            error = "\"path\" resolves to " + have + ", outside the workspace root " + want +
                    "; pass a path inside the root";
            return false;
        }
    }

    resolved = canonical.string();
    return true;
}

namespace {

/// Single-line preview of a longer blob, for model-facing error messages.
std::string preview(const std::string& text, std::size_t budget = 60) {
    std::string flat;
    for (const char c : text) {
        if (flat.size() >= budget) return flat + "...";
        switch (c) {
            case '\n': flat += "\\n"; break;
            case '\t': flat += "\\t"; break;
            case '\r': flat += "\\r"; break;
            default: flat.push_back(c); break;
        }
    }
    return flat;
}

std::string describe(const Value& value, const char* key) {
    return std::string("\"") + key + "\" must be a " +
           (value.is_string() ? "string" : value.is_number_integer() ? "integer" : value.is_boolean() ? "boolean"
                                                                                        : "different type");
}

bool require_string(const Value& args, const char* key, std::string& out, std::string& error) {
    const Value* value = find(args, key);
    if (!value || value->is_null()) {
        error = std::string("\"") + key + "\" is required and must be a string";
        return false;
    }
    if (!value->is_string()) {
        error = describe(*value, key);
        return false;
    }
    out = value->get<std::string>();
    return true;
}

bool optional_int(const Value& args, const char* key, std::int64_t fallback, std::int64_t low,
                  std::int64_t high, std::int64_t& out, std::string& error) {
    const Value* value = find(args, key);
    out = fallback;
    if (!value || value->is_null()) return true;
    if (!value->is_number_integer()) {
        error = describe(*value, key);
        return false;
    }
    out = value->get<std::int64_t>();
    if (out < low || out > high) {
        std::ostringstream os;
        os << "\"" << key << "\" must be between " << low << " and " << high << ", got " << out;
        error = os.str();
        return false;
    }
    return true;
}

bool optional_bool(const Value& args, const char* key, bool fallback, bool& out, std::string& error) {
    const Value* value = find(args, key);
    out = fallback;
    if (!value || value->is_null()) return true;
    if (!value->is_boolean()) {
        error = describe(*value, key);
        return false;
    }
    out = value->get<bool>();
    return true;
}

bool looks_binary(const std::string& text) { return text.find('\0') != std::string::npos; }

/// Reads at most `cap` bytes, reporting whether the file was larger.
bool read_capped(const std::string& path, std::size_t cap, std::string& text, std::uint64_t& total,
                 bool& truncated, std::string& error) {
    std::error_code ec;
    if (!fs::exists(path, ec)) {
        error = "no such file: " + path;
        return false;
    }
    if (fs::is_directory(path, ec)) {
        error = "\"" + path + "\" is a directory; name a file";
        return false;
    }

    // Ask for the size first so the caller can say "showing N of M bytes".
    const std::uintmax_t size = fs::file_size(path, ec);
    const std::uint64_t size_on_disk = ec ? 0 : static_cast<std::uint64_t>(size);
    ec.clear();

    std::ifstream stream(path, std::ios::binary);
    if (!stream) {
        error = "cannot open " + path + ": " + std::strerror(errno);
        return false;
    }

    // One extra byte, so "exactly at the cap" is distinguishable from "over it".
    std::string buffer;
    buffer.resize(cap + 1);
    stream.read(&buffer[0], static_cast<std::streamsize>(cap + 1));
    const std::streamsize got = stream.gcount();
    buffer.resize(static_cast<std::size_t>(got > 0 ? got : 0));

    total = std::max<size_t>(static_cast<size_t>(size_on_disk), buffer.size());
    truncated = total > buffer.size();
    if (truncated) buffer.resize(cap);
    text = std::move(buffer);
    return true;
}

/// Writes `content` to `path`, creating parent directories. Non-append writes go
/// through a temporary file and a rename, so a reader never sees half a file and
/// a failed write leaves the previous contents in place.
bool write_file(const std::string& path, const std::string& content, bool append, std::string& error) {
    std::error_code ec;
    const fs::path target(path);
    if (target.has_parent_path() && !fs::exists(target.parent_path(), ec)) {
        fs::create_directories(target.parent_path(), ec);
        if (ec) {
            error = "cannot create " + target.parent_path().string() + ": " + ec.message();
            return false;
        }
    }

    if (append) {
        std::ofstream stream(path, std::ios::binary | std::ios::app);
        if (!stream) {
            error = "cannot open " + path + " for appending: " + std::strerror(errno);
            return false;
        }
        stream.write(content.data(), static_cast<std::streamsize>(content.size()));
        stream.close();
        if (!stream) {
            error = "write to " + path + " failed";
            return false;
        }
        return true;
    }

    // Unique per process and per call, so concurrent writes never share a temp.
    static std::atomic<unsigned long> counter{0};
    const fs::path temp(path + ".ggmcp-" + std::to_string(static_cast<unsigned long>(::getpid())) + "-" +
                        std::to_string(counter.fetch_add(1)) + ".tmp");

    {
        std::ofstream stream(temp, std::ios::binary | std::ios::trunc);
        if (!stream) {
            error = "cannot open " + temp.string() + " for writing: " + std::strerror(errno);
            return false;
        }
        stream.write(content.data(), static_cast<std::streamsize>(content.size()));
        stream.close();
        if (!stream) {
            error = "write to " + temp.string() + " failed";
            std::filesystem::remove(temp, ec);
            return false;
        }
    }

    fs::rename(temp, target, ec);
    if (ec) {
        std::filesystem::remove(temp, ec);
        error = "cannot replace " + path + ": " + ec.message();
        return false;
    }
    return true;
}

struct PlannedEdit {
    std::size_t position = 0;
    std::string old_text;
    std::string new_text;
};

/// Validates every edit against the *original* text and applies them in one
/// pass. All-or-nothing: one bad edit means nothing is written, which is what
/// makes a multi-edit call safe to retry.
bool plan_edits(const std::string& text, const Value& edits, std::string& out, std::string& error) {
    if (!edits.is_array()) {
        error = "\"edits\" must be an array of {oldText, newText} objects";
        return false;
    }
    const Array& edits_array = edits.get_ref<const Array&>();
    if (edits_array.empty()) {
        error = "\"edits\" must contain at least one edit";
        return false;
    }

    std::vector<PlannedEdit> planned;
    planned.reserve(edits_array.size());

    for (std::size_t i = 0; i < edits_array.size(); ++i) {
        const Value& edit = edits_array[i];
        const std::string where = "edits[" + std::to_string(i) + "]";
        if (!edit.is_object()) {
            error = where + " must be an object with oldText and newText";
            return false;
        }

        const Value* old_value = find(edit, "oldText");
        const Value* new_value = find(edit, "newText");
        if (!old_value || !old_value->is_string()) {
            error = where + ".oldText is required and must be a string";
            return false;
        }
        std::string new_text;
        if (new_value && !new_value->is_null()) {
            if (!new_value->is_string()) {
                error = where + ".newText must be a string (use \"\" to delete)";
                return false;
            }
            new_text = new_value->get<std::string>();
        }

        const std::string& old_text = old_value->get<std::string>();
        if (old_text.empty()) {
            error = where + ".oldText must not be empty; an empty search string is ambiguous";
            return false;
        }

        const std::size_t first = text.find(old_text);
        if (first == std::string::npos) {
            error = where + ".oldText was not found: \"" + preview(old_text) + "\"";
            return false;
        }
        if (text.find(old_text, first + 1) != std::string::npos) {
            error = where + ".oldText matches more than once: \"" + preview(old_text) +
                    "\"; widen it until it is unique";
            return false;
        }
        planned.push_back(PlannedEdit{first, old_text, std::move(new_text)});
    }

    std::sort(planned.begin(), planned.end(),
              [](const PlannedEdit& a, const PlannedEdit& b) { return a.position < b.position; });

    for (std::size_t i = 1; i < planned.size(); ++i) {
        const std::size_t previous_end = planned[i - 1].position + planned[i - 1].old_text.size();
        if (planned[i].position < previous_end) {
            error = "edits[" + std::to_string(i - 1) + "] and edits[" + std::to_string(i) +
                    "] overlap; keep the replaced regions disjoint";
            return false;
        }
    }

    std::string result;
    result.reserve(text.size() + 64);
    std::size_t cursor = 0;
    for (const PlannedEdit& edit : planned) {
        result.append(text, cursor, edit.position - cursor);
        result += edit.new_text;
        cursor = edit.position + edit.old_text.size();
    }
    result.append(text, cursor, std::string::npos);

    out = std::move(result);
    return true;
}

struct ShellResult {
    bool started = false;
    bool timed_out = false;
    bool stragglers = false;  ///< a spawned child outlived the command that spawned it
    int exit_code = -1;
    bool truncated = false;
    std::string output;
    std::string error;
};

/// Monotonic seconds, so the shell budget is immune to wall-clock changes.
double monotonic_seconds() {
    struct timespec ts;
    ::clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<double>(ts.tv_sec) + static_cast<double>(ts.tv_nsec) / 1e9;
}

/// Drains `fd` without blocking. Returns false at EOF or on a hard error, true
/// when the pipe is merely empty right now.
bool drain_pipe(int fd, std::string& out, std::size_t max_bytes, bool& truncated) {
    char chunk[4096];
    while (true) {
        const ssize_t got = ::read(fd, chunk, sizeof(chunk));
        if (got > 0) {
            const std::size_t room = max_bytes > out.size() ? max_bytes - out.size() : 0;
            const std::size_t take = std::min(static_cast<std::size_t>(got), room);
            out.append(chunk, take);
            if (take < static_cast<std::size_t>(got)) truncated = true;
            continue;
        }
        if (got == 0) return false;                                // EOF
        if (errno == EINTR) continue;
        if (errno == EAGAIN || errno == EWOULDBLOCK) return true;  // empty for now
        return false;                                              // unusable pipe
    }
}

/// Runs `command` through /bin/sh -c, merging stderr into stdout, with a
/// wall-clock budget. The child gets its own process group so a timeout takes
/// the whole tree down rather than just the shell.
ShellResult run_shell(const std::string& command, int timeout_seconds, std::size_t max_bytes) {
    ShellResult result;

    int fds[2];
    if (::pipe(fds) != 0) {
        result.error = std::string("pipe() failed: ") + std::strerror(errno);
        return result;
    }

    ::fflush(nullptr);  // nothing buffered, but fork() in a threaded process asks for it
    const pid_t pid = ::fork();
    if (pid < 0) {
        ::close(fds[0]);
        ::close(fds[1]);
        result.error = std::string("fork() failed: ") + std::strerror(errno);
        return result;
    }

    if (pid == 0) {
        // Child: only async-signal-safe calls between here and exec().
        ::setpgid(0, 0);
        ::dup2(fds[1], STDOUT_FILENO);
        ::dup2(fds[1], STDERR_FILENO);
        ::close(fds[0]);
        ::close(fds[1]);
        ::execl("/bin/sh", "sh", "-c", command.c_str(), static_cast<char*>(nullptr));
        ::_exit(127);
    }

    result.started = true;
    ::close(fds[1]);
    ::setpgid(pid, pid);  // races the child's own call; either way the group exists
    ::fcntl(fds[0], F_SETFL, ::fcntl(fds[0], F_GETFL, 0) | O_NONBLOCK);

    double deadline = monotonic_seconds() + static_cast<double>(timeout_seconds);
    double reap_deadline = 0.0;       // child alive, output pipe closed
    double straggler_deadline = 0.0;  // child dead, a straggler holds the pipe
    bool killed = false;
    bool child_reaped = false;
    int status = 0;

    while (true) {
        const bool pipe_open = drain_pipe(fds[0], result.output, max_bytes, result.truncated);

        if (!child_reaped) {
            const pid_t reaped = ::waitpid(pid, &status, WNOHANG);
            if (reaped == pid) {
                child_reaped = true;
            } else if (reaped < 0) {
                ::close(fds[0]);
                result.error = std::string("waitpid() failed: ") + std::strerror(errno);
                return result;
            }
        }

        if (child_reaped) {
            // The command itself is finished. Keep draining only briefly: a
            // backgrounded grandchild can hold the write end open forever, and
            // that must not turn a successful command into a timeout.
            if (pipe_open && straggler_deadline == 0.0) straggler_deadline = monotonic_seconds() + 0.1;
            if (!pipe_open || monotonic_seconds() >= straggler_deadline) {
                if (pipe_open) result.stragglers = true;
                break;
            }
            ::usleep(5000);
            continue;
        }

        if (!pipe_open) {
            // Output is done but the child is still running; reap on a bounded
            // sleep instead of spinning on POLLHUP.
            if (reap_deadline == 0.0) reap_deadline = std::min(deadline, monotonic_seconds() + 1.0);
            if (monotonic_seconds() >= reap_deadline) {
                ::killpg(pid, SIGKILL);
                result.timed_out = true;
                break;
            }
            ::usleep(5000);
            continue;
        }

        const double now = monotonic_seconds();
        if (now >= deadline) {
            if (killed) break;  // already dead; stop waiting on a stray pipe holder
            ::killpg(pid, SIGKILL);
            killed = true;
            result.timed_out = true;
            deadline = monotonic_seconds() + 0.25;  // grace to collect partial output
            continue;
        }

        struct pollfd pfd;
        pfd.fd = fds[0];
        pfd.events = POLLIN;
        pfd.revents = 0;
        const double slice = std::min(0.02, deadline - now);
        ::poll(&pfd, 1, slice > 0.0 ? static_cast<int>(slice * 1000.0) + 1 : 0);
    }

    ::close(fds[0]);
    // Safe to block: by now the process group has been reaped or killed.
    if (!child_reaped && ::waitpid(pid, &status, 0) < 0 && errno != ECHILD) {
        result.error = std::string("waitpid() failed: ") + std::strerror(errno);
        return result;
    }

    if (WIFEXITED(status)) result.exit_code = WEXITSTATUS(status);
    if (WIFSIGNALED(status)) result.exit_code = -WTERMSIG(status);
    return result;
}

/// Strips one trailing newline so tool output does not grow one per round trip.
std::string chomp(std::string text) {
    if (!text.empty() && text.back() == '\n') text.pop_back();
    return text;
}

}  // namespace

std::string FileSystemOptions::root_description() const {
    std::error_code ec;
    const fs::path canonical = fs::canonical(root, ec);
    return ec ? root : canonical.string();
}

void register_filesystem_tools(Dispatcher& dispatcher, const FileSystemOptions& options) {
    const std::string root = options.root_description();

    if (options.enable_file_tools) {
        // --- read ---------------------------------------------------------
        {
            Tool tool;
            tool.name = "read";
            tool.description = std::string("Read a UTF-8 text file inside the workspace root (") + root +
                               ". Returns the contents, optionally sliced by line with offset (1-based) and "
                               "limit. Binary files are refused rather than encoded.";
            tool.input_schema = schema::object(
                {{"path", schema::string("File to read; relative paths resolve against the workspace root.")},
                 {"offset", schema::integer("First line to read, 1-based. Defaults to 1.")},
                 {"limit", schema::integer("Maximum number of lines to return. Defaults to the rest.")}},
                {"path"});
            tool.annotations = tool_annotations("Read a file", /*read_only=*/true, /*destructive=*/false,
                                               /*idempotent=*/true);
            tool.handler = [options, root](const Value& args, const Context&) -> ToolResult {
                std::string path, error;
                if (!require_string(args, "path", path, error)) return ToolResult::error(error);

                std::string resolved;
                if (!confine_path(root, path, options.allow_outside_root, resolved, error))
                    return ToolResult::error(error);

                std::int64_t offset = 1;
                std::int64_t limit = 0;
                if (!optional_int(args, "offset", 1, 1, 1LL << 40, offset, error) ||
                    !optional_int(args, "limit", 0, 1, 1LL << 40, limit, error))
                    return ToolResult::error(error);

                std::string text;
                std::uint64_t total = 0;
                bool truncated = false;
                if (!read_capped(resolved, options.max_read_bytes, text, total, truncated, error))
                    return ToolResult::error(error);

                if (looks_binary(text)) {
                    return ToolResult::error(resolved + " contains a NUL byte; this tool returns text only");
                }

                // Slice by line when asked. offset/limit are line numbers, not bytes.
                if (offset > 1 || limit > 0) {
                    std::vector<std::size_t> starts;
                    starts.push_back(0);
                    for (std::size_t i = 0; i + 1 < text.size(); ++i) {
                        if (text[i] == '\n') starts.push_back(i + 1);
                    }
                    const std::int64_t lines = static_cast<std::int64_t>(starts.size());
                    if (offset > lines) {
                        std::ostringstream os;
                        os << "offset " << offset << " is past the end of a " << lines << "-line file";
                        return ToolResult::error(os.str());
                    }
                    const std::size_t first = static_cast<std::size_t>(offset - 1);
                    const std::size_t wanted = limit > 0 ? static_cast<std::size_t>(limit)
                                                         : static_cast<std::size_t>(lines - first);
                    const std::size_t last = std::min(starts.size(), first + wanted);
                    const std::size_t begin = starts[first];
                    const std::size_t end = last < starts.size() ? starts[last] : text.size();
                    const std::string slice = text.substr(begin, end - begin);

                    std::ostringstream os;
                    os << slice;
                    if (last < starts.size()) {
                        os << "\n[ggmcp: lines " << offset << "-" << last << " of " << lines
                           << "; re-read with offset=" << (last + 1) << " for the rest]";
                    }
                    return ToolResult::text(os.str());
                }

                if (truncated) {
                    std::ostringstream os;
                    os << text << "\n[ggmcp: truncated at " << options.max_read_bytes << " of " << total
                       << " bytes; re-read with offset/limit]";
                    return ToolResult::text(os.str());
                }
                return ToolResult::text(std::move(text));
            };
            dispatcher.add_tool(std::move(tool));
        }

        // --- write --------------------------------------------------------
        {
            Tool tool;
            tool.name = "write";
            tool.description = std::string("Create or overwrite a UTF-8 text file inside the workspace root (") +
                               root + ". Parent directories are created. Replacing an existing file is "
                                      "atomic: a reader never sees a half-written file.";
            tool.input_schema = schema::object(
                {{"path", schema::string("File to create or replace.")},
                 {"content", schema::string("Exact bytes to write. An empty string truncates the file.")},
                 {"append", schema::boolean("Append instead of replacing. Defaults to false.")}},
                {"path", "content"});
            tool.annotations = tool_annotations("Write a file", /*read_only=*/false, /*destructive=*/true,
                                               /*idempotent=*/true);
            tool.handler = [options, root](const Value& args, const Context&) -> ToolResult {
                std::string path, content, error;
                if (!require_string(args, "path", path, error)) return ToolResult::error(error);
                if (!require_string(args, "content", content, error)) return ToolResult::error(error);

                bool append = false;
                if (!optional_bool(args, "append", false, append, error)) return ToolResult::error(error);

                if (content.size() > options.max_write_bytes) {
                    std::ostringstream os;
                    os << "refusing to write " << content.size() << " bytes; the limit is "
                       << options.max_write_bytes;
                    return ToolResult::error(os.str());
                }

                std::string resolved;
                if (!confine_path(root, path, options.allow_outside_root, resolved, error))
                    return ToolResult::error(error);

                if (!write_file(resolved, content, append, error)) return ToolResult::error(error);

                log_line(std::string(append ? "appended " : "wrote ") + std::to_string(content.size()) +
                         " bytes to " + resolved);
                std::ostringstream os;
                os << (append ? "appended " : "wrote ") << content.size() << " bytes to " << resolved;
                return ToolResult::text(os.str());
            };
            dispatcher.add_tool(std::move(tool));
        }

        // --- edit ---------------------------------------------------------
        {
            Tool tool;
            tool.name = "edit";
            tool.description =
                "Edit a file with exact text replacement. `edits` is a list of {oldText, newText}; each "
                "oldText must match exactly one unique, non-overlapping region of the ORIGINAL file. The "
                "call is all-or-nothing: if any edit fails the file is left untouched.";
            tool.input_schema = schema::object(
                {{"path", schema::string("File to edit; must already exist.")},
                 {"edits", schema::array("Replacements to apply in one pass.",
                                         schema::object({{"oldText", schema::string("Unique region to replace.")},
                                                         {"newText", schema::string("Replacement text.")}},
                                                        {"oldText"}))}},
                {"path", "edits"});
            tool.annotations = tool_annotations("Edit a file", /*read_only=*/false, /*destructive=*/true,
                                               /*idempotent=*/false);
            tool.handler = [options, root](const Value& args, const Context&) -> ToolResult {
                std::string path, error;
                if (!require_string(args, "path", path, error)) return ToolResult::error(error);

                std::string resolved;
                if (!confine_path(root, path, options.allow_outside_root, resolved, error))
                    return ToolResult::error(error);

                std::string original;
                std::uint64_t total = 0;
                bool truncated = false;
                if (!read_capped(resolved, options.max_write_bytes, original, total, truncated, error))
                    return ToolResult::error(error);
                if (truncated) {
                    std::ostringstream os;
                    os << "refusing to edit a file larger than " << options.max_write_bytes << " bytes";
                    return ToolResult::error(os.str());
                }
                if (looks_binary(original)) return ToolResult::error("refusing to edit a binary file");

                const Value* edits = find(args, "edits");
                std::string updated;
                if (!plan_edits(original, edits ? *edits : Value(nullptr), updated, error))
                    return ToolResult::error(error);

                if (!write_file(resolved, updated, false, error)) return ToolResult::error(error);

                const std::size_t count = edits && edits->is_array() ? edits->size() : 0;
                log_line("edited " + resolved + " (" + std::to_string(count) + " edits)");
                std::ostringstream os;
                os << "applied " << count << (count == 1 ? " edit" : " edits") << " to " << resolved
                   << " (" << original.size() << " -> " << updated.size() << " bytes)";
                return ToolResult::text(os.str());
            };
            dispatcher.add_tool(std::move(tool));
        }
    }

    if (options.enable_bash) {
        Tool tool;
        tool.name = "bash";
        tool.description =
            "Run a shell command with /bin/sh -c in the server's working directory and return stdout and "
            "stderr merged. This is a real shell with the server's privileges and is NOT confined by the "
            "workspace root; it is only registered when the operator passes --allow-bash.";
        tool.input_schema =
            schema::object({{"command", schema::string("Command line for /bin/sh -c.")},
                            {"timeout", schema::integer("Wall-clock seconds before the process group is "
                                                        "killed. Default " +
                                                        std::to_string(options.bash_timeout_seconds) + ".")}},
                           {"command"});
        tool.annotations = tool_annotations("Run a shell command", /*read_only=*/false, /*destructive=*/true,
                                           /*idempotent=*/false, /*open_world=*/true);
        tool.handler = [options](const Value& args, const Context&) -> ToolResult {
            std::string command, error;
            if (!require_string(args, "command", command, error)) return ToolResult::error(error);
            if (command.empty()) return ToolResult::error("\"command\" must not be empty");

            std::int64_t timeout = options.bash_timeout_seconds;
            if (!optional_int(args, "timeout", options.bash_timeout_seconds, 1, 3600, timeout, error))
                return ToolResult::error(error);

            log_line("bash: " + preview(command, 120));
            const ShellResult shell = run_shell(command, static_cast<int>(timeout), options.max_read_bytes);

            if (!shell.started) return ToolResult::error(shell.error.empty() ? "could not start the command"
                                                                             : shell.error);

            std::ostringstream os;
            os << chomp(shell.output);
            if (shell.timed_out) {
                os << "\n[ggmcp: timed out after " << timeout << "s and killed the process group]";
                return ToolResult::error(os.str());
            }
            if (shell.truncated) {
                os << "\n[ggmcp: output truncated at " << options.max_read_bytes << " bytes]";
            }
            if (shell.stragglers) {
                // The command exited, but something it spawned is still holding
                // the pipe open, so this is as complete as the output gets.
                os << "\n[ggmcp: a background process still holds the output pipe; "
                      "it was left running]";
            }
            if (shell.exit_code != 0) {
                os << "\n[ggmcp: exit " << shell.exit_code << "]";
                return ToolResult::error(os.str());
            }
            return ToolResult::text(os.str());
        };
        dispatcher.add_tool(std::move(tool));
    }
}

}  // namespace ggmcp
