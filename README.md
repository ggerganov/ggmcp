# ggmcp

Model Context Protocol server in C++17. Streamable HTTP, tools only.
Specification: [`docs/spec.md`](docs/spec.md).

## Build

```sh
cmake -S . -B build && cmake --build build -j         # the binary is build/ggmcp
ctest --test-dir build --output-on-failure            # starts the real binary, over sockets
```

## Run

```sh
ggmcp                                           # 0.0.0.0:9932, endpoint /mcp, root $HOME
ggmcp --help                                    # every flag, with its default
ggmcp --alias studio                            # studio-bash, studio-echo, studio-read, ...
ggmcp --instructions "some instructions text"   # set instructions for MCP usage
ggmcp --host 0.0.0.0 --port 9932                # change host/port

GG_TOKEN=$(openssl rand -hex 32) ggmcp --require-token --token-env GG_TOKEN

ggmcp --fs-root ~/projects                      # confine read/write/edit (default: $HOME)
ggmcp --fs-root /                               # the whole disk is then in scope, dotfiles too
ggmcp --allow-outside-root                      # confinement off, the path is taken as given
ggmcp --no-file-tools --no-bash                 # hello, echo, utc_now, whoami only

ggmcp --strict-origin                           # only the Host origin and --allow-origin list
ggmcp --allow-origin '*'                        # any origin, named explicitly
ggmcp --allow-origin http://localhost:3000      # browser agent
ggmcp --no-cors                                 # no CORS headers; browsers can no longer connect

# primary usage
ggmcp --host 0.0.0.0 --port 9932 --fs-root ~/ --alias "my_machine" --instructions "This ggmcp on my_machine"
```

## curl

```sh
H='-H Content-Type:application/json -H Accept:application/json'
U=localhost:9932/mcp

curl -s localhost:9932/health                       # root, policy, alias, tools, sessions

curl -s $U $H -i -d '{"jsonrpc":"2.0","id":1,"method":"initialize",
  "params":{"protocolVersion":"2025-06-18","capabilities":{},"clientInfo":{"name":"curl","version":"0"}}}'
# Mcp-Session-Id: 1c81478c6b701deb00000001f529580747ab844e

S='Mcp-Session-Id: 1c81478c6b701deb00000001f529580747ab844e'
curl -s $U $H -H "$S" -d '{"jsonrpc":"2.0","id":2,"method":"tools/list"}'
curl -s $U $H -H "$S" -d '{"jsonrpc":"2.0","id":3,"method":"tools/call",
  "params":{"name":"mcp-echo","arguments":{"message":"hi"}}}'
# {"id":3,"jsonrpc":"2.0","result":{"content":[{"text":"hi","type":"text"}],"isError":false}}
```

`read`, `write`, `edit` resolve symlinks and stay inside `--fs-root`, which is a
boundary, not a sandbox: `bash` runs anything the server user can run, and there is
no list of paths the file tools will not touch.
