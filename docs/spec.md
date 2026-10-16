# ggmcp — specification

A Model Context Protocol server in C++17, served over streamable HTTP.

## Overview

- Implements MCP protocol versions `2024-11-05`, `2025-03-26`, `2025-06-18` (newest offered)
- Exposes tools only — no resources, prompts, sampling, elicitation or SSE
- JSON and HTTP each come from a replaceable backend behind an interface
- No TLS in the server itself
- Two transports: streamable HTTP (default) and stdio
- Browser agents are served over CORS, any origin allowed by default

## Transports

- HTTP: `POST /mcp` carries JSON-RPC; `Mcp-Session-Id` issued on `initialize`
- Session policy: `lenient` (default), `strict`, `stateless`
- stdio: no session handling, one message per line
- Notifications answer `202` with an empty body; `GET /mcp` answers `405`
- `OPTIONS` on a known path answers `204` as a CORS preflight, or `405` under `--no-cors`;
  any other verb on a known path answers `405`
- JSON-RPC errors go out at HTTP `200`; non-2xx is reserved for transport faults

## Methods

- `initialize` — handshake, echoes the negotiated version, issues a session id
- `notifications/initialized` — completes the handshake
- `ping` — empty result
- `tools/list` — the registered tools and their schemas
- `tools/call` — runs a tool, returns content blocks
- `resources/list`, `prompts/list` — empty lists

## Tools

Always on:

- `hello` — greets, optional `name` and `loud`
- `echo` — returns `message` unchanged
- `utc_now` — UTC time as ISO-8601 and epoch millis
- `whoami` — describes the live session

On by default, disabled with `--no-file-tools`:

- `read` — reads a text file under the root, optional `offset`/`limit` by line
- `write` — creates or replaces a file, atomic, optional `append`
- `edit` — exact text replacements, all-or-nothing

On by default, disabled with `--no-bash`:

- `bash` — `/bin/sh -c` with a timeout; not confined by the root

## Confinement

- File tools resolve symlinks, then test containment against `--fs-root`
- Default root is the home directory of the invoking user
- `--allow-outside-root` drops confinement entirely

## Authorization

- Off by default; `--require-token` makes the server a bearer-token resource server
- Token sources: `--token-env NAME` (default `$GGMCP_TOKEN`), `--token-file PATH`, `--token-stdin`
- No flag takes a token on the command line
- Token file is re-read on change — rotation needs no restart
- Missing or unusable token: `401` + `WWW-Authenticate` + RFC 9728 resource metadata
- Token in the query string is refused even when correct

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

## Implementation details

- The default JSON backend sits behind an interface, so no backend type reaches
  the rest of the server and it can be swapped without touching anything above it
- The default HTTP backend is wrapped the same way: the server speaks its own
  request and response types, and the backend underneath stays replaceable
- The current JSON backend is [nlohmann/json](https://github.com/nlohmann/json)
- The current HTTP backend is [cpp-httplib](https://github.com/yhirose/cpp-httplib)

## Not implemented

- SSE streams and server-originated messages

## End-to-end tests

The server is started as a real process and driven over the wire.

- `GET /health` and the index page
- Requests made before the handshake
- `initialize` and the session id it issues
- `tools/list` and `tools/call`
- `ping` and unknown methods
- Keep-alive and chunked bodies
- Session teardown with `DELETE`
- Session policies: lenient, strict, stateless
- Command-line flag validation
- File and shell tools against a temporary root
- Tools offered by default and under `--no-file-tools --no-bash`
- Origin policies, including `--strict-origin`
- CORS preflight and headers, the `401` path and `--no-cors`
- Bearer tokens, including the `401` path
- The stdio transport
