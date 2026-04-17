#!/usr/bin/env python3
"""Minimal fake MCP stdio server for tests.

Speaks newline-delimited JSON-RPC 2.0:
  - initialize         -> ok
  - tools/list         -> two fake tools ("echo", "add")
  - tools/call echo    -> returns args.text
  - tools/call add     -> returns args.a + args.b
  - tools/call explode -> returns a JSON-RPC error object
Notifications (no "id") are ignored.
"""
import json
import sys


def respond(msg_id, result=None, error=None):
    out = {"jsonrpc": "2.0", "id": msg_id}
    if error is not None:
        out["error"] = error
    else:
        out["result"] = result
    sys.stdout.write(json.dumps(out) + "\n")
    sys.stdout.flush()


def main():
    for line in sys.stdin:
        line = line.strip()
        if not line:
            continue
        req = json.loads(line)
        method = req.get("method")
        msg_id = req.get("id")  # None for notifications

        if msg_id is None:
            continue  # notification, no reply

        if method == "initialize":
            respond(msg_id, {
                "protocolVersion": "2024-11-05",
                "capabilities": {"tools": {}},
                "serverInfo": {"name": "fake-mcp", "version": "0.0.1"},
            })
        elif method == "tools/list":
            respond(msg_id, {"tools": [
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
            ]})
        elif method == "tools/call":
            params = req.get("params", {})
            name = params.get("name")
            args = params.get("arguments", {})
            if name == "echo":
                respond(msg_id, {"content": [{"type": "text", "text": args.get("text", "")}]})
            elif name == "add":
                respond(msg_id, {"content": [{"type": "text",
                                              "text": str(args["a"] + args["b"])}]})
            elif name == "explode":
                respond(msg_id, error={"code": -32000, "message": "boom"})
            else:
                respond(msg_id, error={"code": -32601, "message": f"unknown tool: {name}"})
        else:
            respond(msg_id, error={"code": -32601, "message": f"unknown method: {method}"})


if __name__ == "__main__":
    main()
