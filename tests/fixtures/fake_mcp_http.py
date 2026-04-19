#!/usr/bin/env python3
"""Minimal fake MCP HTTP server for tests.

One endpoint: POST / accepts a JSON-RPC request, returns the response inline
as a JSON body. No streaming. Same semantics as fake_mcp_stdio.py.

Prints the listening port to stdout on startup so tests can pick it up.
"""
from __future__ import annotations

import json
import sys
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer


def _handle(req: dict) -> dict | None:
    method = req.get("method")
    msg_id = req.get("id")
    if msg_id is None:
        return None
    if method == "initialize":
        return {"jsonrpc": "2.0", "id": msg_id,
                "result": {
                    "protocolVersion": "2024-11-05",
                    "capabilities": {"tools": {}},
                    "serverInfo": {"name": "fake-mcp-http", "version": "0.0.1"},
                }}
    if method == "tools/list":
        return {"jsonrpc": "2.0", "id": msg_id,
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
                ]}}
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
    def log_message(self, format, *args):  # silence
        pass

    def do_POST(self):  # noqa: N802
        if self.path != "/":
            self.send_response(404); self.end_headers(); return
        length = int(self.headers.get("Content-Length", "0"))
        body = self.rfile.read(length).decode("utf-8") if length > 0 else ""
        try:
            req = json.loads(body)
        except Exception:
            self.send_response(400); self.end_headers(); return
        reply = _handle(req)
        if reply is None:
            # Notification: 202 with no body, per JSON-RPC convention.
            self.send_response(202); self.end_headers(); return
        payload = json.dumps(reply).encode("utf-8")
        self.send_response(200)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(payload)))
        self.end_headers()
        self.wfile.write(payload)


def main():
    server = ThreadingHTTPServer(("127.0.0.1", 0), Handler)
    port = server.server_address[1]
    sys.stdout.write(f"{port}\n")
    sys.stdout.flush()
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        pass


if __name__ == "__main__":
    main()
