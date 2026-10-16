# ggmcp — specification

A Model Context Protocol server in C++17, served over streamable HTTP.

## Overview

- Implements MCP protocol versions `2024-11-05`, `2025-03-26`, `2025-06-18`
  (newest offered)
- Exposes tools only — no resources, prompts, sampling, elicitation or SSE
- No TLS in the server itself
- Streamable HTTP transport
- Browser agents are served over CORS, any origin allowed by default

## Transport

- HTTP: `POST /mcp` carries JSON-RPC; `Mcp-Session-Id` issued on `initialize`
- Session policy: `lenient` (default), `strict`, `stateless`
- Notifications answer `202` with an empty body; `GET /mcp` answers `405`
- `DELETE /mcp` closes a session
- One instance per port: a second `ggmcp` on a port that is already serving
  fails to bind and exits

## Methods

- `initialize` — handshake, echoes the negotiated version, issues a session id
- `notifications/initialized` — completes the handshake
- `ping` — empty result
- `tools/list` — the registered tools and their schemas
- `tools/call` — runs a tool, returns content blocks
- `resources/list`, `prompts/list` — empty lists

## Tools

Every tool is published as `<alias>-<tool>`, so `bash` reaches the wire as
`mcp-bash`. The alias defaults to `mcp`, `--alias ALIAS` changes it and an empty
`--alias` publishes the bare names. `tools/list` publishes the prefixed names and
`tools/call` accepts only what it published; a bare name is an unknown tool, and
the error says which prefix this server publishes.

Always on:

- `hello` — greets, optional `name` and `loud`
- `echo` — returns `message` unchanged
- `utc_now` — UTC time as ISO-8601 and epoch millis
- `whoami` — describes the live session

On by default, disabled with `--no-file-tools`:

- `read` — reads a file under the root by line, optional `offset`/`limit`
- `write` — creates or replaces a file, optional `append`. A replace is atomic
- `edit` — exact text replacements, all-or-nothing

On by default, disabled with `--no-bash`:

- `bash` — `/bin/sh -c` with a timeout, `command` required and `timeout`
  optional; not confined by the root

## Confinement

- File tools resolve symlinks, then test containment against `--fs-root`
- Default root is the home directory of the invoking user
- `--allow-outside-root` drops confinement entirely
- A relative path resolves against the root
- `~` is not expanded: `~/notes.txt` is `notes.txt` under a directory literally
  named `~` inside the root
- A symlink that leaves the root is refused, and the error quotes the requested
  path rather than the resolved one
- `read` returns the bytes it finds: it does not reject binary or non UTF-8 content

## Authorization

- Off by default; `--require-token` makes the server a bearer-token resource server
- Token sources: `--token-env NAME` reads `$NAME`; with no flag the variable is
  `$GGMCP_TOKEN`; `--token-file PATH` and `--token-stdin` read a file or a line
- No flag takes a token on the command line
- Binds `0.0.0.0:9932`, endpoint `/mcp`, any `Origin` accepted
- `--strict-origin` restores the DNS-rebinding check; `--allow-origin` names
  origins, `*` stands for any
- `GET /health` and a `GET /` index page; both behind the token when auth is on

## Process

- `--help` and `--version` print and exit `0`
- A bad flag, an unusable root, an unreadable token source or a port already in use
  print `ggmcp: <problem>` to stderr and exit `2`
- `SIGINT` and `SIGTERM` close the listener and exit `0`; `SIGPIPE` is ignored, so a
  client that hangs up does not take the server with it
- Unless `--quiet`, start up prints the endpoint, both backends, the session policy,
  the root, the authorization source, the alias, the instructions and the tools

## Implementation details

- CMake (3.16 or newer) is the build system, and the sources are C++17:
  `cmake -S . -B build`, `cmake --build build`, `ctest --test-dir build`
- [nlohmann/json](https://github.com/nlohmann/json) and
  [cpp-httplib](https://github.com/yhirose/cpp-httplib) are vendored as headers under
  `vendor/`, so a build needs no network

## Not implemented

- SSE streams and server-originated messages, and `Last-Event-ID` resumption
- TLS in the server: put a TLS terminator in front of it for anything off the box
- JSON-RPC batching: a JSON array is `-32600 invalid request`
- Resources and prompts exist only as the empty lists above
- Sampling, elicitation, roots and logging
- Sessions are never expired or capped: `initialize` adds one and only `DELETE` takes
  it away

## End-to-end tests (non-exhaustive)

- `GET /health` and the index page
- Requests made before the handshake
- `initialize` and the session id it issues, and a second `initialize` on a live session
- `serverInfo`, its title and the handshake instructions, with and without the flags
- `tools/list` and `tools/call`
- `ping`, unknown methods, a batched array and a broken body
- Keep-alive and chunked bodies
- Session teardown with `DELETE`
- Session policies: lenient, strict, stateless
- Command-line flag validation
- A second instance on a serving port, which must fail to bind
- The three error shapes: a JSON-RPC object at `200`, a string fault, a numeric one
- A non-default `--endpoint`, including what the index page advertises
- File and shell tools against a temporary root
- Writes that fail: the root gains no stray file, the target is untouched, no
  directory is made under a file
- Tools offered by default and under `--no-file-tools --no-bash`
- Tool name alias: default `mcp`, `--alias`, empty alias, unknown bare name
- Origin policies, including `--strict-origin`
- CORS preflight and headers, the `401` path and `--no-cors`
- Bearer tokens, including the `401` challenge, a token in the query string
