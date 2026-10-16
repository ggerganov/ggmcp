# ggmcp — specification

A Model Context Protocol server in C++17, served over streamable HTTP.

## Overview

- Implements MCP protocol versions `2024-11-05`, `2025-03-26`, `2025-06-18` (newest offered)
- Exposes tools only — no resources, prompts, sampling, elicitation or SSE
- No TLS in the server itself
- Two transports: streamable HTTP (default) and stdio
- Browser agents are served over CORS, any origin allowed by default

## Transports

- HTTP: `POST /mcp` carries JSON-RPC; `Mcp-Session-Id` issued on `initialize`
- Session policy: `lenient` (default), `strict`, `stateless`
- stdio: no session handling, one message per line
- Notifications answer `202` with an empty body; `GET /mcp` answers `405`
- `DELETE /mcp` closes a session: `200` closed, `404` for an id this server did not
  issue, `400` when no id is sent
- A body over the size limit is a `413` with an empty body, a non-JSON `Content-Type`
  a `415`, an `Accept` that excludes `application/json` a `406`; a missing `Accept` or
  `*/*` is accepted
- One instance per port: a second `ggmcp` on a port that is already serving fails to
  bind and exits `2` rather than sharing it
- `OPTIONS` on a known path answers `204` as a CORS preflight, or `405` under `--no-cors`;
  any other verb on a known path answers `405`
- JSON-RPC errors go out at HTTP `200`; non-2xx is reserved for transport faults

## Status codes

There are three error shapes, and a client has to know all of them. A JSON-RPC error
goes out at `200` as `{"error":{"code":-32602,"message":"…"},"id":…,"jsonrpc":"2.0"}`.
A protocol fault goes out at a non-2xx with `error` as a **string**, and no `id`:
`{"error":"session_not_found","message":"…"}`. A routing fault goes out at a non-2xx
with `error` as the **status number**: `{"error":405,"message":"method not allowed"}`.

| Status | When |
| --- | --- |
| `200` | every JSON-RPC reply, result or error |
| `202` | a notification, empty body |
| `204` | CORS preflight |
| `400` | `session_required`, `session_not_initialized`, `unexpected_session_id`, `unsupported_protocol_version`, or `bad_request` for a `DELETE` with no id |
| `401` | the token is missing, wrong, or in the query string; the body is the RFC 9728 document and `WWW-Authenticate` carries the challenge |
| `403` | origin refused (routing fault, so `error` is the number) |
| `404` | `session_not_found`, an id this server did not issue |
| `405` | a verb this server does not register, and `OPTIONS` under `--no-cors` (routing fault) |
| `406` | `not_acceptable`, `Accept` excludes `application/json` |
| `413` | body over the size limit, empty body |
| `415` | `unsupported_media_type`, the body is not `application/json` |

## Methods

- `initialize` — handshake, echoes the negotiated version, issues a session id
- `notifications/initialized` — completes the handshake
- `ping` — empty result
- `tools/list` — the registered tools and their schemas
- `tools/call` — runs a tool, returns content blocks
- `resources/list`, `prompts/list` — empty lists
- `initialize` on a live session is a `400 unexpected_session_id`: there is no
  re-handshake, close the session with `DELETE` and start a new one
- A request that carries `MCP-Protocol-Version` must name a version this server
  speaks, otherwise it is a `400 unsupported_protocol_version`; the header is optional
- An unknown method is `-32601` with the method in `data`, a JSON array (batching) is
  `-32600`, an empty or unparsable body `-32700` — all at `200`

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
- `write` — creates or replaces a file, optional `append`. A replace is atomic: a
  reader sees the old content or the new, never a mixture, and a write that cannot be
  completed leaves the file exactly as it was
- `edit` — exact text replacements, all-or-nothing

On by default, disabled with `--no-bash`:

- `bash` — `/bin/sh -c` with a timeout, `command` required and `timeout` optional;
  not confined by the root

### Tool results

`tools/list` publishes each tool with a JSON Schema `inputSchema`, and the names it
publishes are the only names `tools/call` takes. A successful call returns one text
content block holding the tool's own text or JSON.

A tool that fails is **not** a protocol error: the result carries `isError: true` and
the text is `{"error":"…"}`. Only a wrong request is a JSON-RPC error — `params`
without a string `name`, or an unknown name, is `-32602` and lists what is available.

## Confinement

- File tools resolve symlinks, then test containment against `--fs-root`
- Default root is the home directory of the invoking user
- `--allow-outside-root` drops confinement entirely
- A relative path resolves against the root; an absolute path is taken as given and
  accepted when it resolves inside the root
- `~` is not expanded: `~/notes.txt` is `notes.txt` under a directory literally named
  `~` inside the root
- A symlink that leaves the root is refused, and the error quotes the requested path
  rather than the resolved one
- `read` returns the bytes it finds: it does not reject binary or non UTF-8 content

## Authorization

- Off by default; `--require-token` makes the server a bearer-token resource server
- Token sources: `--token-env NAME` reads `$NAME`; with no flag the variable is
  `$GGMCP_TOKEN`; `--token-file PATH` and `--token-stdin` read a file or a line
- No flag takes a token on the command line
- Rotating the token file takes effect without a restart
- Missing or unusable token: `401` + `WWW-Authenticate` + RFC 9728 resource metadata
- `token` or `access_token` in the query string is a `401` even when a correct
  `Authorization` header is sent with it — the query is the problem, not the token
- No usable token at start up is fatal: the server exits `2` rather than serving open

## Browser clients

- CORS is on by default and any origin is allowed, so a web agent connects with no flags
- The preflight answers `204` with the allowed methods, the requested headers (or a
  default list) and `Access-Control-Max-Age`
- A response echoes the requesting origin in `Access-Control-Allow-Origin` and adds
  `Vary: Origin`; the origin is echoed rather than `*`
- `Access-Control-Expose-Headers` names `Mcp-Session-Id` and `MCP-Protocol-Version` —
  without them a browser cannot read the session id it was issued
- The preflight is never authorized: it carries no token by definition
- `401` carries the CORS headers too, so a browser can read the challenge
- `--no-cors` stops sending the headers and turns the preflight back into a `405`, for
  clients that are not browsers

## Defaults

- Binds `0.0.0.0:8081`, endpoint `/mcp`, any `Origin` accepted
- `--strict-origin` restores the DNS-rebinding check; `--allow-origin` names origins,
  `*` stands for any
- A refused origin is a `403` with no CORS headers, so the browser sees nothing
- `GET /health` and a `GET /` index page; both behind the token when auth is on

## Process

- `--help` and `--version` print and exit `0`
- A bad flag, an unusable root, an unreadable token source or a port already in use
  print `ggmcp: <problem>` to stderr and exit `2`
- `SIGINT` and `SIGTERM` close the listener and exit `0`; `SIGPIPE` is ignored, so a
  client that hangs up does not take the server with it
- Unless `--quiet`, start up prints the endpoint, both backends, the session policy,
  the root, the authorization source, the alias and the tools

## Implementation details

- CMake (3.16 or newer) is the build system, and the sources are C++17:
  `cmake -S . -B build`, `cmake --build build`, `ctest --test-dir build`
- [nlohmann/json](https://github.com/nlohmann/json) and
  [cpp-httplib](https://github.com/yhirose/cpp-httplib) are vendored as headers under
  `vendor/`, so a build needs no network
- A bearer token is compared in constant time
- The HTTP library asks for `SO_REUSEPORT` by default, which would let two instances
  share a port and split the traffic between them, so the server overrides that with
  `SO_REUSEADDR` alone: a restart over a socket in `TIME_WAIT` still works

## Not implemented

- SSE streams and server-originated messages, and `Last-Event-ID` resumption
- TLS in the server: put a TLS terminator in front of it for anything off the box
- JSON-RPC batching: a JSON array is `-32600 invalid request`
- Resources and prompts exist only as the empty lists above
- Sampling, elicitation, roots and logging
- Sessions are never expired or capped: `initialize` adds one and only `DELETE` takes
  it away

## End-to-end tests

The server is started as a real process and driven over the wire. Every case is its own
`ctest` entry, and `e2e_all` runs the whole set again in one process.

- `GET /health` and the index page, and what the index page advertises
- `initialize` and the session id it issues, and a second `initialize` on a live session
- Requests made before the handshake: lenient serves them, strict answers `400
  session_not_initialized` until `notifications/initialized` arrives
- `serverInfo`, its title and the handshake instructions, with and without the flags
- The negotiated version echoed back, and an `MCP-Protocol-Version` this server does not
  speak
- `tools/list` and `tools/call`, the two agreeing on every name, and the schema each tool
  publishes
- `ping`, unknown methods, a batched array, a broken body, and `resources/list` and
  `prompts/list` answering empty
- A tool that fails, which is `isError` and not a protocol error
- The transport codes: `202`, `405`, `406`, `413`, `415`, and a path that is not there
- Session teardown with `DELETE`: closed, an id this server did not issue, and no id
- Session policies: lenient, strict, stateless, and stdio ignoring the policy
- Keep-alive across requests, and a body arriving in several chunks. Pipelining is not
  covered: the HTTP library answers only the first of two requests written before either
  answer is read
- The three error shapes: a JSON-RPC object at `200`, a string fault, a numeric one
- A non-default `--endpoint`
- Command-line flag validation, `--help`, `--version`, and a bad flag leaving exit `2`
- The start-up banner, and `--quiet` holding it back
- A second instance on a serving port, which must fail to bind, and a restart over a
  socket left in `TIME_WAIT`, which must work
- `SIGINT` and `SIGTERM` on both transports, and a client that hangs up mid-request
- File and shell tools against a temporary root
- Confinement: a symlink that leaves the root, `~` taken as a directory, a root outside
  the tree taken as given, and bytes that are not UTF-8
- Writes that fail: the root gains no stray file, the target is untouched, no directory
  is made under a file
- Tools offered by default and under `--no-file-tools --no-bash`
- Tool name alias: default `mcp`, `--alias`, empty alias, unknown bare name
- Origin policies, `--strict-origin`, and a refused origin carrying no CORS headers
- CORS preflight and headers, the `401` path and `--no-cors`
- Bearer tokens: `$GGMCP_TOKEN` by default, the `401` challenge, the metadata document
  left public, a token in the query string, rotation of a token file without a restart,
  and an unusable source at start up
- The stdio transport: one message per line, a notification answered with nothing, and
  the session policy ignored
