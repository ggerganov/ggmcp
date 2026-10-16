# ggmcp

Model Context Protocol server in C++17. Streamable HTTP and stdio, tools only.
Specification: [`docs/spec.md`](docs/spec.md).

## Build

```sh
cmake -S . -B build && cmake --build build -j         # the binary is build/ggmcp
ctest --test-dir build --output-on-failure            # starts the real binary, over sockets
```

## Run

```sh
ggmcp                                             # 0.0.0.0:8081, endpoint /mcp, root $HOME
ggmcp --help                                      # every flag, with its default
ggmcp --port 9090 --fs-root ~/work --session-policy strict
ggmcp --transport stdio --no-bash                 # one JSON-RPC message per line
ggmcp --alias studio                              # studio-bash, studio-echo, studio-read, ...
GG_TOKEN=$(openssl rand -hex 32) ggmcp --require-token --token-env GG_TOKEN
ggmcp --host 0.0.0.0 --allow-origin http://localhost:3000   # browser agent
```

## curl

```sh
H='-H Content-Type:application/json -H Accept:application/json'
U=localhost:8081/mcp

curl -s localhost:8081/health                       # root, policy, alias, tools, sessions

curl -s $U $H -i -d '{"jsonrpc":"2.0","id":1,"method":"initialize",
  "params":{"protocolVersion":"2025-06-18","capabilities":{},"clientInfo":{"name":"curl","version":"0"}}}'
# Mcp-Session-Id: 1c81478c6b701deb00000001f529580747ab844e

S='Mcp-Session-Id: 1c81478c6b701deb00000001f529580747ab844e'
curl -s $U $H -H "$S" -d '{"jsonrpc":"2.0","id":2,"method":"tools/list"}'
curl -s $U $H -H "$S" -d '{"jsonrpc":"2.0","id":3,"method":"tools/call",
  "params":{"name":"mcp-echo","arguments":{"message":"hi"}}}'
# {"id":3,"jsonrpc":"2.0","result":{"content":[{"text":"hi","type":"text"}],"isError":false}}
```

Notifications take `202`, JSON-RPC errors still take `200`. Non-2xx is a transport
fault: `401` token, `403` origin, `404` unknown session, `400` policy, `405` method.

## Tools

```
hello(name?, loud?)                 Hello, <name>!
echo(message)                       the message back
utc_now()                           {"epochMillis":…,"iso8601":"…"}
whoami()                            root, confinement, address, transport, session
read(path, offset?, limit?)         lines, offset is 1 based
write(path, content, append?)       atomic replace or append
edit(path, edits[], dryRun?)        all edits apply, or none
bash(command, timeout?)             /bin/sh -c, stdout, stderr, exit code
```

Names reach the wire as `mcp-<tool>`, `tools/call` takes only what `tools/list`
published, and `--alias ''` drops the prefix. `--no-file-tools` drops the last three,
`--no-bash` the last one.

## Guard rails

```sh
ggmcp --fs-root ~/projects              # confine read/write/edit (default: $HOME)
ggmcp --fs-root /                       # the whole disk is then in scope, dotfiles too
ggmcp --allow-outside-root              # confinement off, the path is taken as given
ggmcp --no-file-tools --no-bash         # hello, echo, utc_now, whoami only
```

`read`, `write`, `edit` resolve symlinks and stay inside `--fs-root`, which is a
boundary, not a sandbox: `bash` runs anything the server user can run, and there is
no list of paths the file tools will not touch.

```sh
ggmcp --require-token --token-file ~/.config/ggmcp/token   # hot reloaded
curl -s localhost:8081/.well-known/oauth-protected-resource        # RFC 9728 document
curl -si localhost:8081/health | head -1                           # 401 until you send a token
```

Nothing is behind a token until `--require-token`; never pass a token as a flag or
in a query string.

## Browsers

CORS is on and any origin is allowed, so a browser agent on another host needs no
extra flags.

```sh
ggmcp --strict-origin        # only the Host origin and --allow-origin list
ggmcp --allow-origin '*'     # any origin, named explicitly
ggmcp --no-cors              # no CORS headers; browsers can no longer connect
```

## Not implemented

SSE, server-originated messages, TLS, JSON-RPC batching, resources, prompts,
sampling, elicitation.
