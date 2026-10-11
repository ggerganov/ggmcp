# ggmcp

A [Model Context Protocol](https://modelcontextprotocol.io) server in dependency-free
C++17, served over **streamable HTTP**. No nlohmann/json, no cpp-httplib, no Boost —
just the standard library and POSIX sockets.

Verified against a real MCP client: `pi mcp list` reports
`ggmcp: connected, 4 tools` against this binary — or 8 with the file and shell tools
switched on.

```
$ make && make test
...
100% tests passed out of 2          # 227 in-process checks + 224 live wire checks
```

## Why C++ is a reasonable choice here

It feels like it shouldn't be, because the TypeScript and Python SDKs are large. Those
SDKs are large because they implement *everything* the spec has accumulated (OAuth,
sampling, elicitation, roots, SSE, schema validation). The protocol core that an actual
server needs is small:

| Concern | Size here | Notes |
| --- | --- | --- |
| JSON codec | `src/json.cpp`, ~350 lines | the only genuinely unavoidable hand-rolled piece |
| HTTP/1.1 server | `src/http_server.cpp`, ~530 lines | what "must be HTTP" costs you |
| MCP dispatch | `src/protocol.cpp`, ~420 lines | 5 methods + session state |
| HTTP transport glue | `src/mcp_http.hpp` | headers, Origin, status mapping |
| File + shell tools | `src/filesystem_tools.cpp`, ~790 lines | not protocol at all: confinement, atomic writes, a shell with a timeout and a process group |

So the honest answer: **feasible and pleasant, at the cost of owning a JSON parser and a
small HTTP server.** If your MCP server is a thin wrapper over something already in
C++, this is the whole cost. If you need OAuth or sampling, budget more.

## Build

```bash
cmake -S . -B build && cmake --build build     # or: make
./build/ggmcp --help
```

Requires a C++17 compiler and CMake ≥ 3.16. Nothing else.

## Run

```bash
./build/ggmcp                       # HTTP on http://127.0.0.1:8080/mcp
./build/ggmcp --http 0.0.0.0:9000   # explicit host and port
./build/ggmcp --port 0 --verbose    # ephemeral port, log every request to stderr
./build/ggmcp --no-session          # stateless: never issues a session id
./build/ggmcp --stdio               # newline-delimited JSON-RPC on stdin/stdout
./build/ggmcp --fs-root ./work      # + read/write/edit, confined to ./work
./build/ggmcp --fs-root ./work --allow-bash   # + a real shell (read Security first)
```

Both transports feed the *same* dispatcher, so tool behaviour cannot drift between them.

## Session policy — the thing that breaks naive clients

The spec makes session ids optional *for the server* ("the server MAY assign an MCP
session ID"), so a client that never received one — or that receives one and cannot
retain it — is still a legal caller. Rather than lock such clients out, the server
picks a policy:

| Policy | Flag | Behaviour |
| --- | --- | --- |
| **lenient** (default) | — | issues `Mcp-Session-Id` on `initialize`, but never requires it: a request with no id, or an id it does not recognise, is served against an implicit session |
| strict | `--require-session` | every request after `initialize` must carry a known id, and `notifications/initialized` must have been sent, else `-32002` |
| stateless | `--no-session` | never issues a session id at all, so there is nothing for a client to lose; `DELETE` answers `405` |

The active policy is reported by `GET /health` (`"sessionPolicy"`), echoed in the startup
banner, and `tools/call whoami` answers `"sessionId": "(implicit)"` for a caller that
arrived without one.

Lenient is the default because it is the only setting that serves both kinds of client: a
full MCP client still gets real sessions (clientInfo, `DELETE`, per-session state), while
a client that posts `tools/list` cold gets its tool list instead of:

```json
{"error":{"code":-32002,"message":"Unknown or missing Mcp-Session-Id; POST an initialize first"}}
```

If you are writing a client and that is the error you see, the server is not broken — your
client is dropping the header. Run the server with `--no-session` if you would rather not
implement session retention at all.

## Tools

Always on:

| Tool | Arguments | Returns |
| --- | --- | --- |
| `hello` | `name?` string, `loud?` bool | `"Hello, <name>! (served over http by ggmcp/0.1.0)"` |
| `echo` | `message` string (required) | the message, byte-for-byte |
| `utc_now` | — | `{"iso8601": ..., "weekday": ..., "epochMillis": ...}` |
| `whoami` | — | transport, session id, peer address, clientInfo |

Off until you ask, because they are the ones with consequences:

| Tool | Flag | Arguments | Returns |
| --- | --- | --- | --- |
| `read` | `--fs-root DIR` | `path` (required), `offset?`, `limit?` | the file, or a line slice plus a "lines 4-12 of 40" marker |
| `write` | `--fs-root DIR` | `path`, `content` (required), `append?` | `wrote N bytes to <resolved path>`; creates parents |
| `edit` | `--fs-root DIR` | `path`, `edits[]` of `{oldText, newText?}` | `applied N edits … (before -> after bytes)` |
| `bash` | `--allow-bash` | `command` (required), `timeout?` | merged stdout+stderr; non-zero exit is `isError` |

They mirror the tool names agent harnesses ship, so a client can point at them without
learning a new vocabulary. The semantics that matter:

- **`read`** refuses directories and files containing a NUL byte rather than base64-ing
  them, and caps at 256 KiB, saying so in the returned text.
- **`write`** replaces via a temp file plus `rename`, so a concurrent reader never sees a
  half-written file and a failed write keeps the old contents.
- **`edit`** validates every `oldText` against the *original* text: each must match exactly
  once, and matches may not overlap. One bad match fails the whole call and touches
  nothing, which is what makes a multi-edit call safe to retry.
- **`bash`** runs `/bin/sh -c` in the server's working directory with the server's
  environment. stderr is merged into stdout. A non-zero exit yields `isError` with an
  `exit N` marker. The process gets its own process group, so `--bash-timeout` (or the
  per-call `timeout`) can kill the whole tree rather than just the shell.

## Confinement — what `--fs-root` does and does not buy you

`read`, `write` and `edit` resolve every path with `realpath` **before** the containment
check, then require the result to be the root or below it. Doing it in that order is the
whole point: a lexical check on the raw string is what lets `../../etc/passwd` through, and
it is also what lets a symlink you planted inside the root point straight out of it. Both
are refused, and the refusal never echoes the file's contents back.

It does not buy you anything against `bash`, which is a shell and can `cd` anywhere. That is
why `--allow-bash` is a separate flag: `--fs-root` confines the file tools, and only
explicitly opting out of that default hands out a shell. `--fs-root DIR` must name a
directory that already exists — a typo in a security boundary should fail loudly at startup
rather than create the typo.

## Talking to it

`GET /` prints a live curl recipe; `GET /health` reports status as JSON.

Under the default policy the session header is optional, so the shortest useful request
is just:

```bash
curl -s localhost:8080/mcp -H 'Content-Type: application/json' \
     -d '{"jsonrpc":"2.0","id":1,"method":"tools/list"}'
```

The full handshake, when you want a real session:

```bash
# 1. initialize — capture the Mcp-Session-Id from the response headers
curl -si localhost:8080/mcp -H 'Content-Type: application/json' -d '{
  "jsonrpc": "2.0", "id": 1, "method": "initialize",
  "params": {"protocolVersion": "2025-06-18",
             "clientInfo": {"name": "curl", "version": "0"}, "capabilities": {}}}'

# 2. end the handshake (202 Accepted, empty body by spec)
curl -si localhost:8080/mcp -H 'Content-Type: application/json' \
     -H 'Mcp-Session-Id: <id>' -H 'MCP-Protocol-Version: 2025-06-18' \
     -d '{"jsonrpc":"2.0","method":"notifications/initialized"}'

# 3. list and call tools
curl -s localhost:8080/mcp -H 'Content-Type: application/json' \
     -H 'Mcp-Session-Id: <id>' -H 'MCP-Protocol-Version: 2025-06-18' \
     -d '{"jsonrpc":"2.0","id":2,"method":"tools/call",
          "params":{"name":"hello","arguments":{"name":"Ada","loud":true}}}'
```

Note the `params` wrapper on `tools/call`: the tool name and arguments go *inside*
`params`, not at the top level. Getting that wrong is the single most common way to hand
write an MCP request.

### Wire behaviour

| Request | Result |
| --- | --- |
| `POST /mcp` request with no/unknown session | `200` (lenient) · `404` with `-32002` (strict) |
| `POST /mcp` `initialize` | `200` + `Mcp-Session-Id` header (absent when stateless) |
| `POST /mcp` notification | `202`, empty body |
| `POST /mcp` after handshake | `200` with the JSON-RPC message |
| `GET /mcp` | `405` (this server sends no server-originated messages — legal) |
| `DELETE /mcp` | `200`, session destroyed. Under strict the id then `404`s; under lenient it degrades to an implicit session; `405` when stateless |
| `OPTIONS /mcp` | `204` + `Allow` |
| `GET /health` | `200` JSON status |
| unknown path | `404` JSON |

JSON-RPC-level failures (`-32601` unknown method, `-32602` unknown tool, tool `isError`)
deliberately go out at **HTTP 200** with the detail in the body, so clients display the
message rather than a bare status line. Non-2xx is reserved for transport-level problems:
unparseable JSON (`400`), batching (`400`), bad `MCP-Protocol-Version` (`400`), foreign
`Origin` (`403`), unknown session (`404`), unusable `Accept` (`406`), bad `Content-Type`
(`415`).

## Registering it with a client

pi:

```bash
pi mcp add ggmcp --url http://127.0.0.1:8080/mcp --description "C++ MCP server"
pi mcp list      # ggmcp: connected, 4 tools   (8 with --fs-root + --allow-bash)
```

Any client that speaks streamable HTTP takes the same shape:

```json
{ "mcpServers": { "ggmcp": { "url": "http://127.0.0.1:8080/mcp" } } }
```

A stdio-spawned variant avoids needing a running server first:

```json
{ "mcpServers": { "ggmcp": { "command": "/abs/path/to/build/ggmcp", "args": ["--stdio"] } } }
```

`.pi/mcp.json` in this directory ships the HTTP variant; it is ignored until you trust
the project.

## Layout

```
include/ggmcp/
  json.hpp         JSON value, parser, serializer
  protocol.hpp     Dispatcher, Tool, ToolResult, Reply, JSON-RPC codes
  http_server.hpp  minimal HTTP/1.1 server
  mcp_http.hpp     streamable-HTTP transport, Origin/session rules
  tools.hpp        demo tools, plus the file/shell tools and path confinement
  log.hpp          stderr logging (stdout is reserved for stdio protocol frames)
src/               matching implementations
src/filesystem_tools.cpp   read/write/edit/bash, realpath confinement, timed shell
src/main.cpp       CLI and transport selection
tests/test_main.cpp       in-process JSON + dispatcher checks
tests/http_roundtrip.py   spawns the binary; drives HTTP and stdio end to end
```

## Testing

```bash
make test
# or
python3 tests/http_roundtrip.py ./build/ggmcp
```

The integration test is deliberately paranoid about the parts of HTTP that break silently:
it asserts the `202`-with-no-body notification rule, session issuance and teardown, all
three session policies end to end (including the cold `tools/list` a naive client sends),
rejection of conflicting flags, `Origin` rejection of `http://evil.example`,
`Accept`/`Content-Type` negotiation, id echoing, three requests over one keep-alive
connection, and a `Transfer-Encoding: chunked` request body reassembled in seven-byte
chunks. It also checks that non-ASCII (`café 🚀`, tabs, newlines) survives the round trip
through the hand-rolled JSON codec — the place where a hand-written parser usually leaks.

The file and shell tools are covered from both sides. In-process: `confine_path` on
relative, absolute, `.`/`..`, missing-tree and out-of-root-symlink inputs; the read/write/edit
round trip on a scratch tree; `edit`'s all-or-nothing behaviour confirmed by checking the
bytes on disk after a failed multi-edit call; and `bash` exit codes, stderr merging, timeouts
and backgrounded grandchildren. On the wire: the same calls through real HTTP, plus that a
refused traversal leaks no contents, that `../escape.txt` really does not appear outside the
root, that a timeout comes back in about a second, and that with no flags sent at all the
four file/shell tools are unknown tools (`-32602`), not merely hidden.

The harness spawns each server variant itself and reads the child's stderr to discover its
ephemeral port. Exactly one reader thread owns that pipe: two readers on one pipe is a
deadlock that presents as a hung test run. A `SIGALRM` watchdog turns any future hang into
a traceback instead of a frozen terminal.

## Deliberately not implemented

`2025-06-18` is the newest version supported; `2024-11-05` and `2025-03-26` are accepted
and echoed. Beyond `tools`, the server advertises no capabilities, so these are absent by
design:

- SSE streams and server-originated messages (`GET /mcp` answers `405`)
- resources, prompts, completions, sampling, elicitation, logging
- JSON-RPC batching (deprecated in `2025-03-26`)
- OAuth and bearer-token auth

## Security

Bind to loopback unless something is terminating TLS in front of you: this is plain HTTP
with no authentication of its own. `Origin` is validated against the bound port when a
browser sends one (DNS-rebinding protection, per spec); `--allow-any-origin` turns that
off and you should have a reason. `--host 0.0.0.0` is opt-in, not the default.

Then multiply the stakes by the file and shell tools. An unauthenticated HTTP server that
exposes `write` is a remote file-write primitive; with `--allow-bash` it is unauthenticated
remote code execution as whoever started the process, and `Origin` will not save you — that
header is only sent by browsers, so any `curl` or script on the network simply omits it.
Which is why the defaults are: no file tools, no shell, loopback only, and `--fs-root`
requiring a directory that already exists. Treat `--allow-bash` on a non-loopback bind as
"this machine is in the blast radius", and put a real authenticating proxy in front if that
is what you actually want.

## License

MIT.
