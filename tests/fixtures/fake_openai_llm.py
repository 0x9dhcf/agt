#!/usr/bin/env python3
"""Minimal fake OpenAI-compatible LLM for tests.

Exposes POST /v1/chat/completions. Shape matches OpenAI's response format
closely enough to satisfy agt's llm_openai parser:

  {
    "id": "...",
    "object": "chat.completion",
    "created": 0,
    "model": "<echo model>",
    "choices": [
      {"index": 0,
       "message": {"role": "assistant", "content": "<canned-response>"},
       "finish_reason": "stop"}
    ],
    "usage": {"prompt_tokens": 0, "completion_tokens": 0, "total_tokens": 0}
  }

By default returns "Hello from fake LLM." — enough to let agt::Llm::complete
round-trip without real credentials. Tool-calling is NOT modelled; the point
of this fixture is unit-level coverage of the Llm wrapper, not end-to-end
runner behaviour.

Prints the listening port to stdout on startup.
"""
from __future__ import annotations

import json
import sys
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer


_CANNED_CONTENT = "Hello from fake LLM."


class Handler(BaseHTTPRequestHandler):
    def log_message(self, format, *args):  # silence
        pass

    def do_POST(self):  # noqa: N802
        if self.path != "/v1/chat/completions":
            self.send_response(404); self.end_headers(); return
        length = int(self.headers.get("Content-Length", "0"))
        body = self.rfile.read(length).decode("utf-8") if length > 0 else ""
        try:
            req = json.loads(body) if body else {}
        except Exception:
            self.send_response(400); self.end_headers(); return

        model = req.get("model", "fake-model")
        # Echo the last user message back inside the canned response so tests
        # can verify round-trip delivery without relying on exact equality.
        echoed = ""
        for m in reversed(req.get("messages", [])):
            if m.get("role") == "user":
                c = m.get("content")
                echoed = c if isinstance(c, str) else ""
                break

        resp = {
            "id": "fake-cmpl-0",
            "object": "chat.completion",
            "created": 0,
            "model": model,
            "choices": [
                {
                    "index": 0,
                    "message": {
                        "role": "assistant",
                        "content": f"{_CANNED_CONTENT} (echo: {echoed})",
                    },
                    "finish_reason": "stop",
                }
            ],
            "usage": {"prompt_tokens": 0, "completion_tokens": 0, "total_tokens": 0},
        }
        payload = json.dumps(resp).encode("utf-8")
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
