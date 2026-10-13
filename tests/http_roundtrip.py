#!/usr/bin/env python3
"""End-to-end test for the ggmcp MCP server.

Spawns the real binary and drives the full MCP streamable-HTTP handshake, the
stdio transport, and the optional file/shell tools, using only the Python
standard library.

Usage: http_roundtrip.py /path/to/ggmcp
"""

import http.client
import json
import os
import re
import signal
import socket
import subprocess
import sys
import tempfile
import threading
import time

LATEST = "2025-06-18"

# Tool names, sorted the way the assertions compare them. DEMO_TOOLS is what
# a server offers when the file and shell tools are switched off; the bare
# invocation adds read/write/edit (rooted at $HOME) and bash, because that is
# what `ggmcp` with no flags now means.
DEMO_TOOLS = ["echo", "hello", "utc_now", "whoami"]
DEFAULT_TOOLS = ["bash", "echo", "edit", "hello", "read", "utc_now", "whoami", "write"]

checks = 0
failures = []


def check(condition, what):
    global checks
    checks += 1
    if not condition:
        failures.append(what)
        print(f"FAIL {what}", file=sys.stderr)


def check_eq(got, want, what):
    check(got == want, f"{what}: got {got!r}, want {want!r}")


def check_has(haystack, needle, what):
    check(needle in haystack, f'{what}: expected to contain "{needle}", got {haystack!r}')


class Server:
    """Runs the binary on an ephemeral port and captures its stderr log.

    Exactly one thread ever reads the stderr pipe; it publishes lines into `log`
    and waiters sleep on a condition variable. Two readers on one pipe is a
    silent deadlock: whichever one loses the race blocks in readline() forever.
    """

    def __init__(self, binary, extra_args=(), env=None):
        self.binary = binary
        self.log = []
        self.args = [binary, *extra_args]
        self.env = env
        self._lines = threading.Condition()
        self._reader = None
        self.process = None

    def __enter__(self):
        self.process = subprocess.Popen(
            self.args,
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            bufsize=0,
            env=self.env,
        )
        if self.process.poll() is not None:
            raise RuntimeError(f"server exited immediately: {self.process.stderr.read()!r}")
        self._reader = threading.Thread(target=self._read_stderr, daemon=True)
        self._reader.start()
        return self

    def __exit__(self, *_exc):
        if self.process.poll() is None:
            self.process.terminate()
            try:
                self.process.wait(timeout=5)
            except subprocess.TimeoutExpired:
                self.process.kill()
                self.process.wait(timeout=5)
        if self._reader is not None:
            with self._lines:
                self._lines.notify_all()
            self._reader.join(timeout=5)

    def _read_stderr(self):
        while True:
            line = self.process.stderr.readline()
            if not line:
                break
            text = line.decode("utf-8", "replace").strip()
            with self._lines:
                self.log.append(text)
                self._lines.notify_all()
            print(f"  server | {text}")

    def wait_for_log(self, pattern, timeout=10.0):
        """Waits for a log line matching `pattern`; returns the re.Match."""
        regex = re.compile(pattern)
        deadline = time.time() + timeout
        with self._lines:
            while True:
                for line in self.log:
                    match = regex.search(line)
                    if match:
                        return match
                remaining = deadline - time.time()
                if remaining <= 0:
                    raise RuntimeError(f"no log line matching {pattern!r}; saw {self.log!r}")
                self._lines.wait(remaining)

    def wait_for_port(self, timeout=10.0):
        """Parses the bound port out of the 'listening on ...' log line."""
        match = self.wait_for_log(
            r"listening on http://(?P<host>[\d.]+):(?P<port>\d+)(?P<path>/\S*)", timeout
        )
        return match.group("host"), int(match.group("port")), match.group("path")


def post(host, port, path, body, headers=None, session=None):
    """POSTs one JSON-RPC message. Returns (status, headers, parsed_body_or_text)."""
    connection = http.client.HTTPConnection(host, port, timeout=15)
    request_headers = {"Content-Type": "application/json", "Accept": "application/json, text/event-stream"}
    if session:
        request_headers["Mcp-Session-Id"] = session
    if headers:
        request_headers.update(headers)
    payload = body if isinstance(body, (str, bytes)) else json.dumps(body)
    if isinstance(payload, str):
        payload = payload.encode()
    try:
        connection.request("POST", path, body=payload, headers=request_headers)
        response = connection.getresponse()
        raw = response.read()
        status = response.status
        response_headers = {k.lower(): v for k, v in response.getheaders()}
    finally:
        connection.close()
    parsed = None
    if raw:
        try:
            parsed = json.loads(raw)
        except json.JSONDecodeError:
            parsed = raw.decode("utf-8", "replace")
    return status, response_headers, parsed


def request_raw(method, host, port, path, headers=None, body=b"", session=None):
    connection = http.client.HTTPConnection(host, port, timeout=15)
    request_headers = dict(headers or {})
    if session:
        request_headers["Mcp-Session-Id"] = session
    try:
        connection.request(method, path, body=body, headers=request_headers)
        response = connection.getresponse()
        raw = response.read()
        status = response.status
        response_headers = {k.lower(): v for k, v in response.getheaders()}
    finally:
        connection.close()
    return status, response_headers, raw


def initialize(host, port, path, client_name="pytest"):
    body = {
        "jsonrpc": "2.0",
        "id": 1,
        "method": "initialize",
        "params": {
            "protocolVersion": LATEST,
            "clientInfo": {"name": client_name, "version": "1"},
            "capabilities": {},
        },
    }
    return post(host, port, path, body)


def get_json(host, port, path):
    """GETs a path and parses the response body as JSON. Returns (status, headers, body)."""
    status, headers, raw = request_raw("GET", host, port, path, headers={"Accept": "application/json"})
    try:
        return status, headers, json.loads(raw) if raw else None
    except json.JSONDecodeError:
        return status, headers, raw.decode("utf-8", "replace")


def get_text(host, port, path):
    status, headers, raw = request_raw("GET", host, port, path, headers={"Accept": "text/plain"})
    return status, headers, raw.decode("utf-8", "replace")


def test_health(host, port, path):
    print("\n== GET /health ==")
    status, headers, body = get_json(host, port, "/health")
    check_eq(status, 200, "health status")
    check_has(headers.get("content-type", ""), "application/json", "health content type")
    check(isinstance(body, dict), "health body is JSON")
    if isinstance(body, dict):
        check_eq(body.get("status"), "ok", "health reports ok")
        check_eq(sorted(body.get("tools", [])), DEFAULT_TOOLS, "health lists the default tools")
        check_eq(body.get("server"), "ggmcp", "health names the server")
        check_eq(body.get("endpoint"), "/mcp", "health names the endpoint")


def test_index(host, port, path):
    print("\n== GET / ==")
    status, headers, body = get_text(host, port, "/")
    check_eq(status, 200, "index status")
    check_has(headers.get("content-type", ""), "text/plain", "index is plain text")
    check_has(body, "JSON-RPC", "index explains the protocol")


def test_rejects_before_handshake(host, port, path):
    print("\n== transport-level refusals ==")
    # The default policy is lenient: a request with no Mcp-Session-Id is served
    # rather than rejected, because the spec makes session ids optional for the
    # server. Strict-mode assertions live in test_session_policies().
    status, _, body = post(host, port, path, {"jsonrpc": "2.0", "id": 2, "method": "tools/list"})
    check_eq(status, 200, "tools/list without a session is served by default")
    check_eq(len(body["result"]["tools"]), len(DEFAULT_TOOLS), "and it returns every default tool")

    status, _, body = post(host, port, path, "{ this is not json")
    check_eq(status, 400, "unparseable body is 400")
    check_eq(body["error"]["code"], -32700, "parse error code is -32700")

    status, _, body = post(host, port, path, [{"jsonrpc": "2.0", "id": 3, "method": "ping"}])
    check_eq(status, 400, "a batch is 400")
    check_has(body["error"]["message"], "batching", "batch refusal explains itself")

    status, _, _ = request_raw("GET", host, port, path)
    check_eq(status, 405, "GET on the endpoint is 405")

    status, _, _ = request_raw("PUT", host, port, path, body=b"{}")
    check_eq(status, 405, "PUT on the endpoint is 405")

    status, _, _ = request_raw("GET", host, port, "/nope")
    check_eq(status, 404, "an unknown path is 404")


def test_handshake(host, port, path):
    print("\n== handshake ==")
    status, headers, body = initialize(host, port, path)
    check_eq(status, 200, "initialize status")
    check(body.get("result"), "initialize returns a result")
    check_eq(body["result"]["protocolVersion"], LATEST, "negotiated protocol version")
    check_eq(body["result"]["serverInfo"]["name"], "ggmcp", "serverInfo.name")
    check("tools" in body["result"]["capabilities"], "tools capability advertised")

    session = headers.get("mcp-session-id")
    check(bool(session), "initialize issues Mcp-Session-Id")

    # The notification must be accepted with an empty 202 body.
    status, headers, body = post(
        host, port, path, {"jsonrpc": "2.0", "method": "notifications/initialized"}, session=session
    )
    check_eq(status, 202, "notifications/initialized is 202")
    check_eq(body, None, "notifications/initialized has an empty body")
    return session


def test_tools_list(host, port, path, session):
    print("\n== tools/list ==")
    status, _, body = post(host, port, path, {"jsonrpc": "2.0", "id": 4, "method": "tools/list"}, session=session)
    check_eq(status, 200, "tools/list status")
    tools = body["result"]["tools"]
    check_eq(len(tools), len(DEFAULT_TOOLS), "eight tools")
    # The server lists tools in registration order, so compare as a set.
    check_eq(sorted(tool["name"] for tool in tools), DEFAULT_TOOLS, "tool names")
    for tool in tools:
        schema = tool["inputSchema"]
        check_eq(schema["type"], "object", f'{tool["name"]} schema type')
        check("properties" in schema, f'{tool["name"]} schema properties')
        check(isinstance(schema.get("required"), list), f'{tool["name"]} schema required list')


def call_tool(host, port, path, session, name, arguments, request_id=5, headers=None):
    """MCP puts the tool name and its arguments under `params`, not at top level."""
    body = {"jsonrpc": "2.0", "id": request_id, "method": "tools/call",
            "params": {"name": name, "arguments": arguments if arguments is not None else {}}}
    return post(host, port, path, body, headers=headers, session=session)


def test_tool_calls(host, port, path, session):
    print("\n== tools/call ==")
    status, _, body = call_tool(host, port, path, session, "hello", {})
    check_eq(status, 200, "hello status")
    text = body["result"]["content"][0]["text"]
    check_has(text, "Hello, world!", "hello with no arguments")

    status, _, body = call_tool(host, port, path, session, "hello", {"name": "Ada Lovelace", "loud": True})
    text = body["result"]["content"][0]["text"]
    check_has(text, "HELLO, ADA LOVELACE!", "loud hello")
    check_has(text, "http", "the greeting names the transport")

    status, _, body = call_tool(host, port, path, session, "echo", {"message": "café 🚀 \n tab\there"})
    check_eq(status, 200, "echo status")
    content = body["result"]["content"][0]
    check_eq(content["type"], "text", "echo content type")
    check_eq(content["text"], "café 🚀 \n tab\there", "echo round-trips UTF-8 verbatim")
    check_eq(body["result"]["isError"], False, "echo is not an error")

    status, _, body = call_tool(host, port, path, session, "utc_now", {})
    payload = json.loads(body["result"]["content"][0]["text"])
    check(re.match(r"^\d{4}-\d{2}-\d{2}T\d{2}:\d{2}:\d{2}Z$", payload["iso8601"]), "iso8601 shape")
    check(abs(payload["epochMillis"] - time.time() * 1000) < 60_000, "epoch millis is current")

    status, _, body = call_tool(host, port, path, session, "whoami", {})
    payload = json.loads(body["result"]["content"][0]["text"])
    check_eq(payload["transport"], "http", "whoami transport")
    check_eq(payload["sessionId"], session, "whoami reports the session id")
    check_eq(payload["clientName"], "pytest", "whoami reports clientInfo.name")

    # A missing required argument is reported inside the result.
    status, _, body = call_tool(host, port, path, session, "echo", {})
    check_eq(status, 200, "missing argument is still 200")
    check_eq(body["result"]["isError"], True, "missing argument sets isError")

    # A wrong type is refused rather than coerced.
    status, _, body = call_tool(host, port, path, session, "hello", {"name": 42})
    check_eq(body["result"]["isError"], True, "wrong argument type sets isError")
    check_has(body["result"]["content"][0]["text"], "must be a string", "wrong type explains itself")

    # Unknown tools are a JSON-RPC error at HTTP 200 so clients show the text.
    status, _, body = call_tool(host, port, path, session, "teleport", {})
    check_eq(status, 200, "unknown tool is 200")
    check_eq(body["error"]["code"], -32602, "unknown tool is -32602")
    check_has(body["error"]["message"], "teleport", "unknown tool names the tool")


def test_ping_and_unknown_method(host, port, path, session):
    print("\n== ping, methods, headers ==")
    status, _, body = post(host, port, path, {"jsonrpc": "2.0", "id": 9, "method": "ping"}, session=session)
    check_eq(status, 200, "ping status")
    check_eq(body["result"], {}, "ping returns an empty result")

    status, _, body = post(
        host, port, path, {"jsonrpc": "2.0", "id": 10, "method": "tools/teleport"}, session=session
    )
    check_eq(body["error"]["code"], -32601, "unknown method is -32601")

    status, _, body = post(host, port, path, {"jsonrpc": "2.0", "id": 11, "method": "resources/list"},
                           session=session)
    check_eq(status, 200, "resources/list is answered")
    check_eq(body["result"], {"resources": []}, "resources/list is empty")

    # An unknown notification must be swallowed with 202, not answered.
    status, _, body = post(host, port, path, {"jsonrpc": "2.0", "method": "notifications/cancelled",
                                             "params": {"requestId": "1", "reason": "test"}}, session=session)
    check_eq(status, 202, "unknown notification is 202")
    check_eq(body, None, "unknown notification has no body")

    status, _, _ = post(host, port, path, {"jsonrpc": "2.0", "id": 12, "method": "ping"},
                        headers={"MCP-Protocol-Version": "1999-01-01"}, session=session)
    check_eq(status, 400, "an unsupported MCP-Protocol-Version is 400")

    status, _, _ = post(host, port, path, {"jsonrpc": "2.0", "id": 13, "method": "ping"},
                        headers={"MCP-Protocol-Version": LATEST}, session=session)
    check_eq(status, 200, "a supported MCP-Protocol-Version is accepted")

    # Accepting any Origin is the default (it is what lets a browser page drive
    # this server); the 403 for a foreign one is asserted under --strict-origin
    # in test_origin_policies(), where it is actually configured to happen.
    status, _, _ = post(host, port, path, {"jsonrpc": "2.0", "id": 14, "method": "ping"},
                        headers={"Origin": "http://evil.example"}, session=session)
    check_eq(status, 200, "a foreign Origin is accepted by default")

    status, _, _ = post(host, port, path, {"jsonrpc": "2.0", "id": 15, "method": "ping"},
                        headers={"Origin": f"http://localhost:{port}"}, session=session)
    check_eq(status, 200, "a loopback Origin is accepted")

    status, _, _ = post(host, port, path, {"jsonrpc": "2.0", "id": 16, "method": "ping"},
                        headers={"Accept": "text/event-stream"}, session=session)
    check_eq(status, 406, "an Accept that excludes JSON is 406")

    status, _, _ = post(host, port, path, {"jsonrpc": "2.0", "id": 17, "method": "ping"},
                        headers={"Content-Type": "text/plain"}, session=session)
    check_eq(status, 415, "a non-JSON Content-Type is 415")

    status, headers, _ = request_raw("OPTIONS", host, port, path)
    check_eq(status, 204, "OPTIONS is 204")
    check_has(headers.get("allow", ""), "POST", "OPTIONS advertises POST")


def test_keep_alive_and_chunked(host, port, path, session):
    print("\n== HTTP details ==")
    # One connection, three requests: proves keep-alive framing.
    connection = http.client.HTTPConnection(host, port, timeout=15)
    for index in range(3):
        connection.request(
            "POST",
            path,
            body=json.dumps({"jsonrpc": "2.0", "id": 100 + index, "method": "ping"}).encode(),
            headers={
                "Content-Type": "application/json",
                "Accept": "application/json",
                "Mcp-Session-Id": session,
            },
        )
        response = connection.getresponse()
        payload = json.loads(response.read())
        check_eq(response.status, 200, f"keep-alive request {index} status")
        check_eq(payload["id"], 100 + index, f"keep-alive request {index} id echoed")
    connection.close()

    # A chunked request body has to be reassembled by the server.
    raw = socket.create_connection((host, port), timeout=15)
    message = json.dumps({"jsonrpc": "2.0", "id": 77, "method": "ping"})
    body = b""
    for start in range(0, len(message), 7):
        piece = message[start:start + 7].encode()
        body += f"{len(piece):x}\r\n".encode() + piece + b"\r\n"
    body += b"0\r\n\r\n"
    head = (
        f"POST {path} HTTP/1.1\r\nHost: {host}:{port}\r\nContent-Type: application/json\r\n"
        f"Accept: application/json\r\nMcp-Session-Id: {session}\r\nTransfer-Encoding: chunked\r\n\r\n"
    )
    raw.sendall(head.encode())
    raw.sendall(body)
    raw.settimeout(15)

    # Read the status line and headers, then exactly Content-Length bytes.
    head = b""
    while b"\r\n\r\n" not in head:
        chunk = raw.recv(4096)
        if not chunk:
            break
        head += chunk
    head_text, _, remainder = head.partition(b"\r\n\r\n")
    length = 0
    for line in head_text.split(b"\r\n")[1:]:
        key, _, value = line.partition(b":")
        if key.decode("latin-1").strip().lower() == "content-length":
            length = int(value.decode("latin-1"))
    body_out = remainder
    while len(body_out) < length:
        chunk = raw.recv(length - len(body_out))
        if not chunk:
            break
        body_out += chunk
    raw.close()

    check_has(head_text.decode("latin-1"), "200", "chunked request returns 200")
    check_has(body_out.decode("latin-1"), '"id":77', "chunked body was reassembled")


def test_session_teardown(host, port, path, session):
    print("\n== DELETE ==")
    status, _, _ = request_raw("DELETE", host, port, path, session=session)
    check_eq(status, 200, "DELETE ends the session")

    # Lenient policy: an id that no longer names a session falls back to an
    # implicit session rather than erroring. Strict mode keeps the 404; that is
    # asserted in test_session_policies().
    status, _, _ = post(host, port, path, {"jsonrpc": "2.0", "id": 20, "method": "ping"}, session=session)
    check_eq(status, 200, "a closed session id degrades to an implicit session")

    status, _, _ = request_raw("DELETE", host, port, path, session=session)
    check_eq(status, 404, "DELETE on a closed session is 404")

    status, _, _ = request_raw("DELETE", host, port, path)
    check_eq(status, 404, "DELETE with no session id is 404")


def run_policy_suite(binary, extra_args, title, checks):
    """Starts the server with extra_args and hands (host, port, path) to `checks`."""
    print(f"\n== session policy: {title} ==")
    with Server(binary, ["--http", "127.0.0.1:0", *extra_args]) as server:
        # wait_for_port() reads stderr itself, so the drainer must not start
        # until it is done: two readers on one pipe means whoever loses the race
        # blocks in readline() forever.
        host, port, path = server.wait_for_port()
        checks(host, port, path)


def test_session_policies(binary):
    """lenient (default) / strict (--require-session) / stateless (--no-session)."""
    list_req = {"jsonrpc": "2.0", "id": 2, "method": "tools/list"}

    def lenient_checks(host, port, path):
        _, _, health = get_json(host, port, "/health")
        check_eq(health.get("sessionPolicy"), "lenient", "health reports lenient")

        status, headers, _ = initialize(host, port, path)
        check_eq(status, 200, "initialize succeeds")
        check(bool(headers.get("mcp-session-id")), "lenient still issues a session id")

        status, _, body = post(host, port, path, list_req)
        check_eq(status, 200, "tools/list with no session id is served")
        check_eq(len(body["result"]["tools"]), len(DEFAULT_TOOLS), "the sessionless list has eight tools")

        status, _, body = post(host, port, path, list_req, session="not-a-real-session")
        check_eq(status, 200, "tools/list with an unknown session id is served")

        # The KiSS-client shape: no handshake at all, straight to a tool call.
        status, _, body = post(host, port, path, {"jsonrpc": "2.0", "id": 3, "method": "tools/call",
                                                 "params": {"name": "hello",
                                                            "arguments": {"name": "KiSS"}}})
        check_eq(status, 200, "tools/call works with no handshake")
        check_has(body["result"]["content"][0]["text"], "Hello, KiSS!", "the tool really ran")

        status, _, body = post(host, port, path, {"jsonrpc": "2.0", "id": 4, "method": "tools/call",
                                                 "params": {"name": "whoami", "arguments": {}}})
        who = json.loads(body["result"]["content"][0]["text"])
        check_eq(who["sessionId"], "(implicit)", "whoami reports the implicit session")
        check_eq(who["transport"], "http", "whoami reports the transport")

    def strict_checks(host, port, path):
        _, _, health = get_json(host, port, "/health")
        check_eq(health.get("sessionPolicy"), "strict", "health reports strict")

        status, _, body = post(host, port, path, list_req)
        check_eq(status, 404, "strict rejects a sessionless request")
        check_eq(body["error"]["code"], -32002, "strict refusal is -32002")

        status, _, body = post(host, port, path, list_req, session="not-a-real-session")
        check_eq(status, 404, "strict rejects an unknown session id")

        status, headers, _ = initialize(host, port, path)
        session = headers.get("mcp-session-id")
        check(bool(session), "strict issues a session id")

        status, _, body = post(host, port, path, list_req, session=session)
        check_eq(status, 200, "strict answers before the notification...")
        check_has(body["error"]["message"], "not initialized", "...with a JSON-RPC error, not a status")

        status, _, _ = post(host, port, path, {"jsonrpc": "2.0", "id": 5, "method": "ping"}, session=session)
        check_eq(status, 200, "strict answers ping before the notification")

        post(host, port, path, {"jsonrpc": "2.0", "method": "notifications/initialized"}, session=session)
        status, _, body = post(host, port, path, list_req, session=session)
        check_eq(status, 200, "strict serves tools/list after the handshake")
        check_eq(len(body["result"]["tools"]), len(DEFAULT_TOOLS), "the strict list has eight tools")

        request_raw("DELETE", host, port, path, session=session)
        status, _, _ = post(host, port, path, {"jsonrpc": "2.0", "id": 6, "method": "ping"}, session=session)
        check_eq(status, 404, "strict rejects a closed session")

    def stateless_checks(host, port, path):
        _, _, health = get_json(host, port, "/health")
        check_eq(health.get("sessionPolicy"), "stateless", "health reports stateless")
        check_eq(health.get("sessions"), 0, "a stateless server stores no sessions")

        status, headers, body = initialize(host, port, path)
        check_eq(status, 200, "stateless initialize succeeds")
        check(not headers.get("mcp-session-id"), "stateless issues no session id")
        check_eq(body["result"]["protocolVersion"], LATEST, "stateless still negotiates a version")

        status, _, body = post(host, port, path, list_req)
        check_eq(status, 200, "stateless serves tools/list")
        check_eq(len(body["result"]["tools"]), len(DEFAULT_TOOLS), "the stateless list has eight tools")

        status, _, _ = request_raw("DELETE", host, port, path, session="anything")
        check_eq(status, 405, "stateless refuses DELETE - there is no session to close")

    run_policy_suite(binary, [], "lenient (default)", lenient_checks)
    run_policy_suite(binary, ["--require-session"], "strict", strict_checks)
    run_policy_suite(binary, ["--no-session"], "stateless", stateless_checks)


def test_flag_validation(binary):
    print("\n== flag validation ==")
    for args, want in ((["--require-session", "--no-session"], 2),
                       (["--session-policy", "sideways"], 2),
                       (["--session-policy=sideways"], 2)):
        process = subprocess.run([binary, *args], capture_output=True, text=True, timeout=10)
        check_eq(process.returncode, want, f"{' '.join(args)} exits {want}")
        check_has(process.stderr, "session", "the error mentions sessions")

    for flag in ("--require-session", "--no-session"):
        process = subprocess.run([binary, flag, "--help"], capture_output=True, text=True, timeout=10)
        check_eq(process.returncode, 0, f"{flag} --help exits 0")
        check_has(process.stdout, "--session-policy", "--help documents --session-policy")

    # The defaults are part of the interface, and --help is where a caller
    # reads them before deciding what to switch off.
    code, out, _ = run_command(binary, ["--help"])
    check_eq(code, 0, "--help exits 0")
    check_has(out, "0.0.0.0:8081", "--help documents the default bind")
    check_has(out, "--no-file-tools", "--help documents --no-file-tools")
    check_has(out, "--no-bash", "--help documents --no-bash")
    check_has(out, "--strict-origin", "--help documents --strict-origin")

    # The file and shell tools need their own kind of validation: a root that
    # cannot be resolved must stop the server rather than quietly widen the scope.
    with tempfile.TemporaryDirectory(prefix="ggmcp-flag-") as root:
        cases = [
            (["--fs-root"], "--fs-root needs a value"),
            (["--fs-root", os.path.join(root, "nowhere")], "cannot be resolved"),
            (["--bash-timeout", "0"], "--bash-timeout must be between 1 and 3600"),
            (["--bash-timeout", "soon"], "--bash-timeout expects a number"),
        ]
        for args, want in cases:
            process = subprocess.run([binary, *args], capture_output=True, text=True, timeout=10)
            check_eq(process.returncode, 2, f"{' '.join(args)} exits 2")
            check_has(process.stderr, want, f"the error for {' '.join(args)}")

        # An existing root starts fine and says so in the banner. bash is on by
        # default, so it takes --no-bash to get a server without a shell in it.
        with Server(binary, ["--http", "127.0.0.1:0", "--fs-root", root, "--no-bash"]) as server:
            host, port, path = server.wait_for_port()
            check_has(server.wait_for_log(r"file tools confined to").group(0), "file tools confined to",
                      "--fs-root logs the root")
            _, _, health = get_json(host, port, "/health")
            check_eq(sorted(health.get("tools", [])),
                     ["echo", "edit", "hello", "read", "utc_now", "whoami", "write"],
                     "--fs-root advertises seven tools")
            check("bash" not in health.get("tools", []), "--no-bash keeps the shell out")

        # --fs-root and --no-file-tools are the two halves of one decision, so
        # the last one typed is the one that stands.
        for args, present in ((["--no-file-tools", "--fs-root", root], True),
                              (["--fs-root", root, "--no-file-tools"], False)):
            with Server(binary, ["--http", "127.0.0.1:0", *args]) as server:
                host, port, path = server.wait_for_port()
                _, _, health = get_json(host, port, "/health")
                check_eq("read" in health.get("tools", []), present,
                         f"{' '.join(args)} leaves read {'' if present else 'out of '}the list")


def tool_text(host, port, path, session, name, arguments):
    """Calls a tool and returns (is_error, text). Tool errors travel in a 200 result."""
    status, _, body = call_tool(host, port, path, session, name, arguments)
    check_eq(status, 200, f"{name} returns HTTP 200")
    if "result" not in body:
        return True, json.dumps(body)
    result = body["result"]
    content = result.get("content") or [{}]
    return bool(result.get("isError")), content[0].get("text", "")


def test_filesystem_tools(binary):
    """--fs-root turns on read/write/edit confined to a directory; --allow-bash adds a shell."""
    print("\n== filesystem tools ==")
    with tempfile.TemporaryDirectory(prefix="ggmcp-fs-") as root, \
            tempfile.TemporaryDirectory(prefix="ggmcp-fs-out-") as outside:
        secret = os.path.join(outside, "secret.txt")
        with open(secret, "w") as handle:
            handle.write("do not read this")
        os.symlink(secret, os.path.join(root, "link.txt"))

        with Server(binary, ["--http", "127.0.0.1:0", "--fs-root", root, "--allow-bash"]) as server:
            host, port, path = server.wait_for_port()
            check_has(server.wait_for_log(r"file tools confined to").group(0), "file tools confined to",
                      "the banner names the confinement root")
            check_has(server.wait_for_log(r"bash is ENABLED").group(0), "bash is ENABLED",
                      "the banner warns that bash is on")

            session = test_handshake(host, port, path)

            status, _, body = post(host, port, path, {"jsonrpc": "2.0", "id": 2, "method": "tools/list"},
                                   session=session)
            names = sorted(tool["name"] for tool in body["result"]["tools"])
            check_eq(names, ["bash", "echo", "edit", "hello", "read", "utc_now", "whoami", "write"],
                     "eight tools with the file and shell tools enabled")

            # write -> read, including directories that do not exist yet.
            is_error, text = tool_text(host, port, path, session, "write",
                                       {"path": "a.txt", "content": "alpha\nbeta\ngamma\n"})
            check(not is_error, "write succeeds over the wire")
            check_has(text, "wrote 17 bytes", "write reports the size")
            with open(os.path.join(root, "a.txt")) as handle:
                check_eq(handle.read(), "alpha\nbeta\ngamma\n", "write reached the filesystem")

            is_error, text = tool_text(host, port, path, session, "write",
                                       {"path": "x/y/z.txt", "content": "nested"})
            check(not is_error, "write creates parent directories")
            check(os.path.isfile(os.path.join(root, "x/y/z.txt")), "the nested file exists")

            is_error, text = tool_text(host, port, path, session, "read", {"path": "a.txt"})
            check(not is_error, "read succeeds")
            check_eq(text, "alpha\nbeta\ngamma\n", "read returns the file verbatim")

            is_error, text = tool_text(host, port, path, session, "read",
                                       {"path": "a.txt", "offset": 2, "limit": 1})
            check(not is_error, "read with offset/limit succeeds")
            check_has(text, "beta", "read honours offset")
            check_has(text, "lines 2-2 of 3", "read reports the slice")

            is_error, text = tool_text(host, port, path, session, "edit", {
                "path": "a.txt",
                "edits": [{"oldText": "alpha", "newText": "ALPHA"},
                          {"oldText": "gamma", "newText": "OMEGA"}],
            })
            check(not is_error, "a multi-edit call succeeds")
            with open(os.path.join(root, "a.txt")) as handle:
                check_eq(handle.read(), "ALPHA\nbeta\nOMEGA\n", "both edits landed on disk")

            # A failed edit must leave the file exactly as it was.
            is_error, text = tool_text(host, port, path, session, "edit",
                                       {"path": "a.txt", "edits": [{"oldText": "A", "newText": "X"}]})
            check(is_error, "an ambiguous oldText is a tool error")
            check_has(text, "more than once", "the ambiguity is explained")
            with open(os.path.join(root, "a.txt")) as handle:
                check_eq(handle.read(), "ALPHA\nbeta\nOMEGA\n", "a failed edit changed nothing")

            # Confinement.
            is_error, text = tool_text(host, port, path, session, "read", {"path": "../../etc/passwd"})
            check(is_error, "a traversal out of the root is refused")
            check_has(text, "outside the workspace root", "the traversal error names the root")

            is_error, text = tool_text(host, port, path, session, "read", {"path": "/etc/passwd"})
            check(is_error, "an absolute out-of-root path is refused")

            is_error, text = tool_text(host, port, path, session, "read", {"path": "link.txt"})
            check(is_error, "a symlink out of the root is refused")
            check("do not read this" not in text, "the refusal leaks nothing")

            is_error, text = tool_text(host, port, path, session, "write",
                                       {"path": "../escape.txt", "content": "x"})
            check(is_error, "write refuses to leave the root")
            check(not os.path.exists(os.path.join(os.path.dirname(root), "escape.txt")),
                  "nothing was written outside the root")

            # bash
            is_error, text = tool_text(host, port, path, session, "bash", {"command": "echo wired"})
            check(not is_error, "bash runs a command")
            check_eq(text, "wired", "bash captures stdout")

            is_error, text = tool_text(host, port, path, session, "bash",
                                       {"command": "echo o; echo e >&2; exit 7"})
            check(is_error, "a failing command sets isError")
            check_has(text, "o", "bash keeps stdout on failure")
            check_has(text, "e", "bash merges stderr")
            check_has(text, "exit 7", "bash reports the exit code")

            started = time.time()
            is_error, text = tool_text(host, port, path, session, "bash", {"command": "sleep 30", "timeout": 1})
            elapsed = time.time() - started
            check(is_error, "a timeout is an error")
            check_has(text, "timed out", "the timeout is explained")
            check(1.0 <= elapsed < 5.0, f"the timeout fired promptly (took {elapsed:.2f}s)")

            is_error, text = tool_text(host, port, path, session, "bash",
                                       {"command": "(sleep 30 &) ; echo spawned", "timeout": 5})
            check(not is_error, "a backgrounded grandchild does not fail the command")
            check_has(text, "spawned", "the straggler's parent output survives")

            is_error, text = tool_text(host, port, path, session, "bash", {"command": "printf x", "timeout": 0})
            check(is_error, "an out-of-range timeout is rejected")


def test_default_tools(binary):
    """A bare invocation offers the file and shell tools; --no-* take them back."""
    print("\n== tool defaults ==")
    # --port 0 keeps the default host (every interface) without squatting on the
    # default port, which may already have a ggmcp of the user's on it.
    with Server(binary, ["--port", "0"]) as server:
        host, port, path = server.wait_for_port()
        check_eq(host, "0.0.0.0", "the default bind is every interface")
        check_has(server.wait_for_log(r"file tools confined to").group(0), "file tools confined to",
                  "the default file root is logged")
        check_has(server.wait_for_log(r"bash is ENABLED").group(0), "bash is ENABLED",
                  "bash is on without being asked for")
        session = test_handshake(host, port, path)
        status, _, body = post(host, port, path, {"jsonrpc": "2.0", "id": 2, "method": "tools/list"},
                               session=session)
        check_eq(status, 200, "the default server answers tools/list")
        check_eq(sorted(tool["name"] for tool in body["result"]["tools"]), DEFAULT_TOOLS,
                 "the bare server offers eight tools")

    # Each switch takes away exactly what it names, and nothing else.
    for args, want in (
        (["--no-file-tools"], ["bash", *DEMO_TOOLS]),
        (["--no-bash"], ["echo", "edit", "hello", "read", "utc_now", "whoami", "write"]),
        (["--no-file-tools", "--no-bash"], DEMO_TOOLS),
    ):
        with Server(binary, ["--port", "0", *args]) as server:
            host, port, path = server.wait_for_port()
            _, _, health = get_json(host, port, "/health")
            check_eq(sorted(health.get("tools", [])), want,
                     f"{' '.join(args)} leaves exactly {len(want)} tools")
            if "--no-file-tools" in args:
                check(not any("file tools confined to" in line for line in server.log),
                      "--no-file-tools does not even resolve the default root")

    # And a tool that was switched off is a tool that does not exist.
    with Server(binary, ["--port", "0", "--no-file-tools", "--no-bash"]) as server:
        host, port, path = server.wait_for_port()
        session = test_handshake(host, port, path)
        for name in ("read", "write", "edit", "bash"):
            status, _, body = call_tool(host, port, path, session, name, {"path": "a.txt", "command": "echo"})
            check_eq(status, 200, f"{name} is not reachable at the HTTP layer")
            check_eq(body["error"]["code"], -32602, f"{name} is an unknown tool")


def test_origin_policies(binary):
    """Any-Origin is the default; --strict-origin and --allow-origin take it back."""
    print("\n== origin policies ==")

    def ping_with_origin(host, port, path, origin, session):
        status, _, _ = post(host, port, path, {"jsonrpc": "2.0", "id": 30, "method": "ping"},
                            headers={"Origin": origin}, session=session)
        return status

    with Server(binary, ["--port", "0", "--strict-origin"]) as server:
        host, port, path = server.wait_for_port()
        session = test_handshake(host, port, path)
        check_eq(ping_with_origin(host, port, path, f"http://localhost:{port}", session), 200,
                 "strict accepts loopback on the bound port")
        check_eq(ping_with_origin(host, port, path, f"http://127.0.0.1:{port}", session), 200,
                 "strict accepts 127.0.0.1 on the bound port")
        check_eq(ping_with_origin(host, port, path, "http://evil.example", session), 403,
                 "strict rejects a foreign origin")

    # Naming an origin implies strict: otherwise the list would be dead
    # configuration against the any-Origin default.
    with Server(binary, ["--port", "0", "--allow-origin", "http://good.example"]) as server:
        host, port, path = server.wait_for_port()
        session = test_handshake(host, port, path)
        check_eq(ping_with_origin(host, port, path, "http://good.example", session), 200,
                 "a named origin is accepted")
        check_eq(ping_with_origin(host, port, path, "http://evil.example", session), 403,
                 "naming one origin turns the any-Origin default off")


def test_stdio(binary):
    print("\n== stdio transport ==")
    process = subprocess.Popen([binary, "--stdio"], stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                               stderr=subprocess.PIPE, bufsize=0)

    def send(message):
        process.stdin.write((json.dumps(message) + "\n").encode())
        process.stdin.flush()

    def receive():
        line = process.stdout.readline()
        return json.loads(line) if line else None

    send({"jsonrpc": "2.0", "id": 1, "method": "initialize",
          "params": {"protocolVersion": LATEST, "clientInfo": {"name": "stdio-test", "version": "1"},
                     "capabilities": {}}})
    reply = receive()
    check_eq(reply["result"]["protocolVersion"], LATEST, "stdio initialize")

    send({"jsonrpc": "2.0", "method": "notifications/initialized"})
    # Notifications must produce no output at all; tools/list arriving next
    # proves the notification was not answered.
    send({"jsonrpc": "2.0", "id": 2, "method": "tools/list"})
    reply = receive()
    check_eq(sorted(tool["name"] for tool in reply["result"]["tools"]), DEFAULT_TOOLS,
             "stdio tools/list carries the default tools")

    send({"jsonrpc": "2.0", "id": 3, "method": "tools/call",
          "params": {"name": "hello", "arguments": {"name": "stdio"}}})
    reply = receive()
    check_has(reply["result"]["content"][0]["text"], "Hello, stdio!", "stdio tool call")
    check_has(reply["result"]["content"][0]["text"], "stdio by", "stdio names its transport")

    send({"jsonrpc": "2.0", "id": 4, "method": "no/such/method"})
    reply = receive()
    check_eq(reply["error"]["code"], -32601, "stdio unknown method")

    process.stdin.close()
    check_eq(process.wait(timeout=10), 0, "stdio exits 0 on EOF")


def initialize_with_token(host, port, path, headers, client_name="pytest"):
    return post(
        host, port, path,
        {"jsonrpc": "2.0", "id": 1, "method": "initialize",
         "params": {"protocolVersion": LATEST, "clientInfo": {"name": client_name, "version": "1"},
                    "capabilities": {}}},
        headers=headers)


def run_command(binary, args, env=None, stdin=None):
    """Runs the binary to completion; returns (returncode, stdout, stderr)."""
    process = subprocess.run([binary, *args], capture_output=True, text=True, timeout=10, env=env,
                             input=stdin)
    return process.returncode, process.stdout, process.stderr


def test_authorization(binary):
    """Bearer-token authorization: the 401 handshake, token sources, rotation."""
    print("\n== authorization ==")
    good = "wire-test-token-aaaaaaaaaaaa"
    bad = "wire-test-token-zzzzzzzzzzzz"
    metadata_path = "/.well-known/oauth-protected-resource"

    with tempfile.TemporaryDirectory(prefix="ggmcp-auth-") as root:
        token_file = os.path.join(root, "tokens.txt")
        with open(token_file, "w") as handle:
            handle.write(f"# one token per line, comments start with #\n\n{good}\n")

        with Server(binary, ["--http", "127.0.0.1:0", "--require-token", "--token-file", token_file]) as server:
            host, port, path = server.wait_for_port()
            bearer = {"Authorization": f"Bearer {good}"}
            tools_list = {"jsonrpc": "2.0", "id": 1, "method": "tools/list"}

            check_has(server.wait_for_log(r"authorization required").group(0), "authorization required",
                      "the banner says authorization is required")
            check_has(server.wait_for_log(r"unauthenticated requests get 401").group(0),
                      "unauthenticated requests get 401", "the banner names the consequence")

            # --- the 401 handshake -------------------------------------------
            status, headers, body = post(host, port, path, tools_list)
            check_eq(status, 401, "a request with no credential is refused")
            challenge = headers.get("www-authenticate", "")
            check_has(challenge, "Bearer realm=", "the challenge offers the Bearer scheme")
            check_has(challenge, 'error="invalid_request"', "a missing credential is invalid_request")
            check_has(challenge, "resource_metadata=", "the challenge points at the metadata document")
            check_has(challenge, metadata_path, "the challenge points at the right document")
            check_eq(headers.get("cache-control"), "no-store", "a 401 is not cacheable")
            check_eq(headers.get("x-content-type-options"), "nosniff", "a 401 says nosniff")
            check(isinstance(body, dict) and "error_description" in str(body), "the 401 body explains itself")

            status, headers, body = post(host, port, path, tools_list, headers={"Authorization": f"Bearer {bad}"})
            check_eq(status, 401, "a wrong token is refused")
            check_has(headers.get("www-authenticate", ""), 'error="invalid_token"',
                      "a wrong token is invalid_token, not invalid_request")
            check(bad not in json.dumps(body) and good not in json.dumps(body),
                  "the refusal echoes neither the tried token nor a real one")

            status, headers, _ = post(host, port, path, tools_list, headers={"Authorization": "Basic Zm9vOmJhcg=="})
            check_eq(status, 401, "another auth scheme is refused")
            check_has(headers.get("www-authenticate", ""), "Bearer", "the refusal names the scheme it wants")

            status, headers, _ = post(host, port, path, tools_list, headers={"Authorization": f"bearer {good}"})
            check_eq(status, 200, "the Bearer scheme is matched case-insensitively")

            # --- a good token works everywhere a bad one fails ----------------
            status, _, body = post(host, port, path, tools_list, headers=bearer)
            check_eq(status, 200, "a valid token is accepted")
            check_eq(len(body["result"]["tools"]), len(DEFAULT_TOOLS), "a valid token sees the tools")

            status, headers, _ = initialize(host, port, path)
            check_eq(status, 401, "initialize without a token is refused")
            status, headers, _ = initialize_with_token(host, port, path, bearer)
            check_eq(status, 200, "initialize with a token is accepted")
            check(headers.get("mcp-session-id", "") != "", "initialize with a token still issues a session id")

            # --- /health, / and the metadata document ------------------------
            status, _, _ = get_json(host, port, "/health")
            check_eq(status, 401, "/health is behind the token too: which tools exist is configuration")
            status, _, health = request_raw("GET", host, port, "/health", headers=bearer)
            check_eq(status, 200, "/health with a token is served")
            check_has(json.loads(health).get("authorization", ""), "bearer", "health reports bearer auth")

            status, _, index = get_text(host, port, "/")
            check_eq(status, 401, "the index page is behind the token")

            status, _, index = request_raw("GET", host, port, "/", headers=bearer)
            check_eq(status, 200, "the index page with a token is served")
            check_has(index.decode(), "Authorization is required", "the index tells callers to send a token")

            # The metadata document is the one thing an unauthenticated client
            # must be able to read - that is what it is for.
            status, _, document = get_json(host, port, metadata_path)
            check_eq(status, 200, "the metadata document needs no token")
            check(document.get("resource", "").endswith(path), "the metadata names this resource")
            check("header" in document.get("bearer_methods_supported", []), "the metadata advertises header bearer")
            check("authorization_servers" not in document,
                  "no authorization server is advertised when none was configured")

            # --- no back door through the query string -----------------------
            status, _, body = post(host, port, f"{path}?access_token={good}", tools_list, headers=bearer)
            check_eq(status, 401, "a valid bearer token does not license a query-string credential")
            check_has(json.dumps(body), "query string", "the refusal says why")

            for name in ("token", "apiKey", "key", "id_token"):
                status, _, _ = post(host, port, f"{path}?{name}={good}", tools_list, headers=bearer)
                check_eq(status, 401, f"?{name}= is refused as a credential channel")

            status, _, _ = post(host, port, f"{path}?name=hello", tools_list, headers=bearer)
            check_eq(status, 200, "an ordinary query parameter is not a credential")

            # --- whoami reports the source, never the secret -----------------
            status, headers, _ = initialize(host, port, path)
            session = headers.get("mcp-session-id")
            check_eq(status, 401, "initialize without a token gets no session")
            status, headers, _ = initialize_with_token(host, port, path, bearer)
            session = headers.get("mcp-session-id", session)
            status, _, body = call_tool(host, port, path, session, "whoami", {}, request_id=2, headers=bearer)
            text = json.dumps(body.get("result", {}))
            # The token sits on line 3 of the file: a comment and a blank line precede it.
            check_has(text, "tokens.txt:3", "whoami names the credential source")
            check(good not in text, "whoami never repeats the credential it was given")

            # --- rotation: rewrite the file, no restart ----------------------
            # The file is only re-read on the way past the door, so the request
            # after the rewrite is what notices.
            with open(token_file, "w") as handle:
                handle.write(f"{bad}\n")
            status, _, _ = post(host, port, path, tools_list, headers=bearer)
            check_eq(status, 401, "the old token stops working once the file changes")
            check_has(server.wait_for_log(r"reloaded").group(0), "reloaded", "the rotation is logged")
            status, _, _ = post(host, port, path, tools_list, headers={"Authorization": f"Bearer {bad}"})
            check_eq(status, 200, "the new token works without a restart")

        # --- two tokens, both honoured ------------------------------------
        two_tokens = os.path.join(root, "two.txt")
        with open(two_tokens, "w") as handle:
            handle.write(f"{good}\n{bad}\n")
        with Server(binary, ["--http", "127.0.0.1:0", "--require-token", "--token-file", two_tokens]) as server:
            host, port, path = server.wait_for_port()
            for token in (good, bad):
                status, _, _ = post(host, port, path, tools_list, headers={"Authorization": f"Bearer {token}"})
                check_eq(status, 200, f"each configured token works ({token[:6]}...)")

        # --- --token-stdin: a secret that never touched argv ---------------
        reader, writer = os.pipe()
        os.write(writer, (good + "\n").encode())
        os.close(writer)
        process = subprocess.Popen([binary, "--http", "127.0.0.1:0", "--require-token", "--token-stdin"],
                                   stdin=reader, stdout=subprocess.PIPE, stderr=subprocess.PIPE, bufsize=0)
        os.close(reader)
        try:
            deadline = time.time() + 10
            port = 0
            while time.time() < deadline and port == 0:
                line = process.stderr.readline().decode().strip()
                if not line:
                    break
                if "listening on" in line:
                    port = int(line.rsplit(":", 1)[1].split("/")[0])
            check(port > 0, "--token-stdin starts the server")
            if port:
                status, _, _ = post("127.0.0.1", port, "/mcp", tools_list, headers=bearer)
                check_eq(status, 200, "the token read from stdin is the one that works")
                status, _, _ = post("127.0.0.1", port, "/mcp", tools_list)
                check_eq(status, 401, "and it is still required")
        finally:
            process.terminate()
            process.wait(timeout=5)

        # --- misconfigurations must fail at startup ------------------------
        bare_env = {k: v for k, v in os.environ.items() if k != "GGMCP_TOKEN"}
        cases = [
            (["--require-token"], bare_env, None, "GGMCP_TOKEN"),
            (["--require-token", "--token-env", "GGMCP_NOT_SET"], None, None, "GGMCP_NOT_SET"),
            (["--require-token", "--token-file", os.path.join(root, "missing.txt")], None, None,
             "could not be opened"),
            (["--require-token", "--token-file", os.path.join(root, "empty.txt")], None, None,
             "contains no tokens"),
            (["--token-file", token_file], None, None, "--require-token"),
            (["--stdio", "--token-stdin"], None, "\n", "--stdio"),
            (["--require-token", "--token-stdin"], None, "", "stdin is empty"),
        ]
        with open(os.path.join(root, "empty.txt"), "w") as handle:
            handle.write("# nothing here but a comment\n")

        for args, env, stdin, want in cases:
            code, _, err = run_command(binary, args, env=env, stdin=stdin)
            check_eq(code, 2, f"{' '.join(args)} exits 2")
            check_has(err, want, f"the error for {' '.join(args)} mentions {want!r}")

        # A short token is not fatal, but it must be said out loud.
        with Server(binary, ["--http", "127.0.0.1:0", "--require-token", "--token-env", "GGMCP_SHORT"],
                    env=dict(os.environ, GGMCP_SHORT="abc")) as server:
            host, port, path = server.wait_for_port()
            check_has(server.wait_for_log(r"character token").group(0), "character token",
                      "a short token is warned about")
            status, _, _ = post(host, port, path, tools_list, headers=bearer)
            check_eq(status, 401, "a short token is still a real check")

        # The default server keeps answering anonymous callers: turning this
        # feature on is what changes the wire, not its mere existence.
        with Server(binary, ["--http", "127.0.0.1:0"], env=bare_env) as server:
            host, port, path = server.wait_for_port()
            status, _, _ = post(host, port, path, tools_list)
            check_eq(status, 200, "without --require-token anonymous calls still work")
            _, _, health = get_json(host, port, "/health")
            check_eq(health.get("authorization"), "anonymous", "health reports anonymous auth by default")
            _, _, document = get_json(host, port, metadata_path)
            check_eq(document.get("resource"), None, "the metadata document is not served when auth is off")


def main():
    # Turn a future hang into a loud failure with a traceback instead of an
    # infinitely spinning test run.
    def _on_alarm(_signum, _frame):
        raise TimeoutError("http_roundtrip exceeded its watchdog budget")

    signal.signal(signal.SIGALRM, _on_alarm)
    signal.alarm(240)

    if len(sys.argv) < 2:
        print(__doc__, file=sys.stderr)
        return 2
    binary = os.path.abspath(sys.argv[1])
    if not os.path.exists(binary):
        print(f"no such binary: {binary}", file=sys.stderr)
        return 2

    with Server(binary, ["--http", "127.0.0.1:0"]) as server:
        host, port, path = server.wait_for_port()
        check_eq(host, "127.0.0.1", "bound host")
        check(path in ("/mcp", "/mcp/"), "bound endpoint path")
        check(port > 0, "bound port is not zero")

        test_health(host, port, path)
        test_index(host, port, path)
        test_rejects_before_handshake(host, port, path)
        session = test_handshake(host, port, path)
        test_tools_list(host, port, path, session)
        test_tool_calls(host, port, path, session)
        test_ping_and_unknown_method(host, port, path, session)
        test_keep_alive_and_chunked(host, port, path, session)
        test_session_teardown(host, port, path, session)

    test_session_policies(binary)
    test_flag_validation(binary)
    test_filesystem_tools(binary)
    test_default_tools(binary)
    test_origin_policies(binary)
    test_authorization(binary)
    test_stdio(binary)

    signal.alarm(0)  # cancel the watchdog
    print()
    if failures:
        print(f"{len(failures)} of {checks} checks FAILED")
        return 1
    print(f"all {checks} checks passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
