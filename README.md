# ggmcp

A Model Context Protocol server in C++17 over streamable HTTP (and stdio). It
exposes tools only, keeps the JSON and HTTP backends behind interfaces, and is
driven end to end by tests that start the real binary and talk to it over a
socket.

The specification this implements is [`docs/spec.md`](docs/spec.md).

## Build

```sh
cmake -S . -B build
cmake --build build -j
ctest --test-dir build --output-on-failure
```

The binary lands in `build/ggmcp`. Both third-party backends are vendored as
headers under `vendor/`, so no download or package manager is needed.

## Run

```sh
./build/ggmcp --help            # the full list of flags
./build/ggmcp                   # 0.0.0.0:8081, endpoint /mcp, root $HOME
./build/ggmcp --port 9090 --fs-root /tmp/work --session-policy strict
./build/ggmcp --transport stdio --no-bash
```

| Flag | Default | Meaning |
| --- | --- | --- |
| `--transport http\|stdio` | `http` | streamable HTTP, or one JSON-RPC message per line |
| `--host ADDR` | `0.0.0.0` | address to bind |
| `--port N` | `8081` | port to bind, `0` picks a free one |
| `--endpoint PATH` | `/mcp` | JSON-RPC endpoint |
| `--session-policy POLICY` | `lenient` | `lenient`, `strict` or `stateless` |
| `--fs-root PATH` | `$HOME` | root the file tools are confined to |
| `--allow-outside-root` | off | drop file tool confinement |
| `--no-file-tools` | off | do not offer `read`, `write`, `edit` |
| `--no-bash` | off | do not offer `bash` |
| `--bash-timeout N` | `30` | bash timeout in seconds |
| `--strict-origin` | off | require `Origin` to match `Host` or `--allow-origin` |
| `--allow-origin ORIGIN` | none | allow an extra origin, repeatable |
| `--require-token` | off | require `Authorization: Bearer <token>` |
| `--token-env NAME` | `$GGMCP_TOKEN` | read the token from an environment variable |
| `--token-file PATH` | none | read the token from a file, re-read when it changes |
| `--token-stdin` | off | read the token from the first line of stdin |
| `--name`, `--title`, `--instructions` | `ggmcp` | `serverInfo` and handshake instructions |
| `--json-backend`, `--http-backend` | `nlohmann`, `httplib` | choose the backends |
| `--quiet`, `--verbose` | stderr log | silence or raise the log level |

No flag takes a token value: tokens come from the environment, a file or stdin.

## Layout

```
include/gg/json.hpp          backend agnostic JSON value, the only JSON type above the backend
include/gg/json_backend.hpp  the JSON backend interface
include/gg/http.hpp          Request / Response / Router / IServer, the only HTTP types
include/gg/backends.hpp      the composition root: which backends are compiled in
include/gg/mcp/              config, protocol, sessions, tools, authorization, transports
src/json/json_nlohmann.cpp   the nlohmann/json backend  (only TU that includes nlohmann)
src/http/httplib_server.cpp  the cpp-httplib backend     (only TU that includes httplib)
vendor/                      vendored headers and their licenses
tests/e2e/                   the end to end suite
```

Two rules keep the backends replaceable:

- `gg::Json` hides its backend node behind `json::IValue`, and
  `src/json/json_nlohmann.cpp` is the only file that includes `<nlohmann/json.hpp>`.
  Everything above sees `gg::Json` and nothing else.
- The HTTP backend turns a wire level request into a `gg::http::Request`, hands it
  to the backend agnostic `gg::http::Router`, and writes back the
  `gg::http::Response` it got. Swapping cpp-httplib means writing one file and
  adding a name to `src/backends.cpp`.

## Protocol

- Versions offered: `2024-11-05`, `2025-03-26`, `2025-06-18` (newest). An unknown
  requested version is answered with the newest one.
- Methods: `initialize`, `notifications/initialized`, `ping`, `tools/list`,
  `tools/call`, `resources/list`, `prompts/list`. Resources and prompts are empty
  lists, and there is no SSE or server-originated traffic.
- JSON-RPC errors always travel at HTTP `200`. Non-2xx is reserved for transport
  faults: `401` authorization, `403` origin, `404` unknown session, `400` session
  policy or framing, `405` method, `406` `Accept`, `415` `Content-Type`.
- Notifications answer `202` with an empty body. `GET /mcp` answers `405`.

### Session policies

| Policy | Behaviour |
| --- | --- |
| `lenient` | a request without `Mcp-Session-Id` runs on an ephemeral session, an unknown id is `404` |
| `strict` | a session id issued by `initialize` and a completed handshake are required, otherwise `400`; an unknown id is `404` |
| `stateless` | no session is ever issued or tracked, an incoming id is ignored |

The stdio transport never tracks sessions: it behaves like `stateless` whatever
`--session-policy` says.

## Tools

Always on:

| Tool | Arguments | Result |
| --- | --- | --- |
| `hello` | `name?`, `loud?` | `Hello, <name>!`, upper cased when `loud` |
| `echo` | `message` | the message unchanged |
| `utc_now` | &mdash; | ISO-8601 stamp and epoch milliseconds |
| `whoami` | &mdash; | describes the live session |

With `--no-file-tools` removed (the default) the file tools join:

| Tool | Arguments | Result |
| --- | --- | --- |
| `read` | `path`, `offset?`, `limit?` | the selected lines, `offset` is 1 based |
| `write` | `path`, `content`, `append?` | atomic replace or append (temp file plus rename) |
| `edit` | `path`, `edits[{oldText,newText}]`, `dryRun?` | every edit applies, or none does |

With `--no-bash` removed, `bash` runs `/bin/sh -c <command>` with a wall clock
timeout and reports stdout, stderr and the exit code. It is not confined by the
root, the other file tools are.

Confinement resolves symlinks (`weakly_canonical`) and then tests containment
against `--fs-root`; `--allow-outside-root` turns the check off.

## Authorization

Off by default. `--require-token` turns the server into a bearer token resource
server: `401` with a `WWW-Authenticate` challenge carrying `error`,
`error_description` and `resource_metadata`, and the RFC 9728 protected resource
document as the body. The same document is served unauthenticated from
`/.well-known/oauth-protected-resource`. `/health` and the index page sit behind
the token, `POST`/`DELETE` on the endpoint do too. A token in the query string is
refused even when it is correct, and a token file is re-read when its `stat`
signature changes, so rotation needs no restart.

## Tests

`ctest --test-dir build --output-on-failure` runs `build/tests/ggmcp-e2e`, which
starts a real `ggmcp` for each case and drives it over a hand written HTTP/1.1
client (keep-alive, chunked bodies, bare sockets) or through stdio pipes. Pass a
substring to run a subset:

```sh
./build/tests/ggmcp-e2e ./build/ggmcp bearer
```

## Not implemented

SSE streams, server-originated messages, TLS in the server itself, JSON-RPC
batching, resources, prompts, sampling, elicitation.
