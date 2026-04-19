#!/usr/bin/env python3
"""Minimal fake MCP SSE server for tests.

Implements the classic SSE transport (MCP 2024-11-05):
  GET  /sse                   → streams events
                                 first event: `event: endpoint\ndata: /messages?sid=<uuid>`
                                 subsequent: `event: message\ndata: <json-rpc-response>`
  POST /messages?sid=<uuid>   → accepts a JSON-RPC request; replies with 202
                                 and publishes the reply on the /sse stream
                                 owned by `sid`.

Same JSON-RPC semantics as fake_mcp_stdio.py (initialize, tools/list, tools/call
with echo / add / explode).

Prints the listening port to stdout on startup so tests can pick it up.
"""
from __future__ import annotations

import json
import queue
import sys
import threading
import uuid
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from typing import Optional
from urllib.parse import parse_qs, urlparse

# Session-id → outbound event queue. Each entry is a tuple (event_name, data).
_sessions: dict[str, "queue.Queue[Optional[tuple[str, str]]]"] = {}
_sessions_lock = threading.Lock()


def _handle_jsonrpc(req: dict) -> Optional[dict]:
    method = req.get("method")
    msg_id = req.get("id")
    if msg_id is None:
        return None  # notifications get no reply
    if method == "initialize":
        return {
            "jsonrpc": "2.0", "id": msg_id,
            "result": {
                "protocolVersion": "2024-11-05",
                "capabilities": {"tools": {}},
                "serverInfo": {"name": "fake-mcp-sse", "version": "0.0.1"},
            },
        }
    if method == "tools/list":
        return {
            "jsonrpc": "2.0", "id": msg_id,
            "result": {"tools": [
                {"name": "echo", "description": "Echoes text back",
                 "inputSchema": {"type": "object",
                                 "properties": {"text": {"type": "string"}},
                                 "required": ["text"]}},
                {"name": "add", "description": "Adds two numbers",
                 "inputSchema": {"type": "object",
                                 "properties": {"a": {"type": "number"},
                                                "b": {"type": "number"}},
                                 "required": ["a", "b"]}},
                {"name": "explode", "description": "Always errors"},
            ]},
        }
    if method == "tools/call":
        params = req.get("params", {})
        name = params.get("name")
        args = params.get("arguments", {})
        if name == "echo":
            return {"jsonrpc": "2.0", "id": msg_id,
                    "result": {"content": [{"type": "text", "text": args.get("text", "")}]}}
        if name == "add":
            return {"jsonrpc": "2.0", "id": msg_id,
                    "result": {"content": [{"type": "text",
                                            "text": str(args["a"] + args["b"])}]}}
        if name == "explode":
            return {"jsonrpc": "2.0", "id": msg_id,
                    "error": {"code": -32000, "message": "boom"}}
        return {"jsonrpc": "2.0", "id": msg_id,
                "error": {"code": -32601, "message": f"unknown tool: {name}"}}
    return {"jsonrpc": "2.0", "id": msg_id,
            "error": {"code": -32601, "message": f"unknown method: {method}"}}


class Handler(BaseHTTPRequestHandler):
    def log_message(self, format, *args):  # noqa: A002 — silence HTTP server log
        pass

    def do_GET(self):  # noqa: N802
        parsed = urlparse(self.path)
        if parsed.path != "/sse":
            self.send_response(404)
            self.end_headers()
            return
        sid = uuid.uuid4().hex
        q: "queue.Queue[Optional[tuple[str, str]]]" = queue.Queue()
        with _sessions_lock:
            _sessions[sid] = q
        try:
            self.send_response(200)
            self.send_header("Content-Type", "text/event-stream")
            self.send_header("Cache-Control", "no-cache")
            self.send_header("Connection", "keep-alive")
            self.end_headers()

            # First event: tell the client where to POST.
            self._emit("endpoint", f"/messages?sid={sid}")

            while True:
                item = q.get()  # blocks
                if item is None:
                    return  # shutdown
                event_name, data = item
                self._emit(event_name, data)
        except (BrokenPipeError, ConnectionResetError):
            pass
        finally:
            with _sessions_lock:
                _sessions.pop(sid, None)

    def do_POST(self):  # noqa: N802
        parsed = urlparse(self.path)
        if parsed.path != "/messages":
            self.send_response(404)
            self.end_headers()
            return
        sid_list = parse_qs(parsed.query).get("sid", [])
        sid = sid_list[0] if sid_list else ""
        with _sessions_lock:
            q = _sessions.get(sid)
        if q is None:
            self.send_response(404)
            self.end_headers()
            return
        length = int(self.headers.get("Content-Length", "0"))
        body = self.rfile.read(length).decode("utf-8") if length > 0 else ""
        try:
            req = json.loads(body)
        except Exception:
            self.send_response(400)
            self.end_headers()
            return

        reply = _handle_jsonrpc(req)
        # Spec: 202 Accepted, no body — the actual reply streams via /sse.
        self.send_response(202)
        self.end_headers()
        if reply is not None:
            q.put(("message", json.dumps(reply)))

    def _emit(self, event: str, data: str) -> None:
        # Real SSE servers (Houston MCP among them) end every line with CRLF.
        # Using CRLF here keeps the test aligned with production shape —
        # catches regressions in the client's CR handling.
        frame = f"event: {event}\r\ndata: {data}\r\n\r\n"
        self.wfile.write(frame.encode("utf-8"))
        self.wfile.flush()


def main():
    server = ThreadingHTTPServer(("127.0.0.1", 0), Handler)
    port = server.server_address[1]
    sys.stdout.write(f"{port}\n")
    sys.stdout.flush()
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        pass
    finally:
        # Release any outstanding SSE streams.
        with _sessions_lock:
            for q in _sessions.values():
                q.put(None)


if __name__ == "__main__":
    main()
