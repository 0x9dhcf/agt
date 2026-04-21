# Changelog

All notable changes to this project are documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [Unreleased]

## [0.6.3] - 2026-04-21

### Fixed
- `SqliteSession`: set `PRAGMA busy_timeout=5000` alongside `journal_mode=WAL`. Without it, concurrent writes to the shared `messages` table (e.g. three rapid-fire mission threads in Mission Control all opening their own session for the same SQLite file) surface `SQLITE_BUSY` as an uncaught `std::runtime_error` and take the process down. Waiting up to 5 seconds for the lock is the standard remedy and matches what Python MC's SQLAlchemy engine already does.

## [0.6.2] - 2026-04-20

### Fixed
- `agt::Http` now serialises access with an internal mutex. The class had always owned a single CURL handle and reused it via `curl_easy_reset`, which is fine for a single-threaded caller but SEGV-prone when multiple threads shared one `agt::Llm` (and therefore one `Http`). Callers that intentionally hold `agt::Llm` at process scope — Mission Control being the motivating case — now hit a serialised gate instead of tearing the handle state.
- `CURLOPT_NOSIGNAL=1` is set on every `curl_easy_perform` path (Http plus the SSE reader in `McpServerImpl`). Libcurl's default DNS timeout uses `SIGALRM`, which is process-global and not safe under threading — this neutralises that race, which manifested as `SEGV in curl_strnequal` during concurrent LLM calls.

## [0.6.1] - 2026-04-18

### Fixed
- `McpServerImpl::sse_write_cb`: SSE frames produced by real servers (Houston MCP among them) end every line with CRLF. The previous implementation mapped every `\r` to `\n` instead of stripping it, which splintered every `\r\n\r\n` boundary into four newlines — the parser then saw the `event: endpoint` and `data: ...` lines as two separate frames and the POST-endpoint URL carried a stray trailing `\r` that libcurl rejected as malformed. Now normalise by erasing `\r`. The SSE test fixture emits CRLF as production servers do to keep this regression caught by tests.

## [0.6.0] - 2026-04-18

### Added
- `McpTransport::sse`: MCP over Server-Sent Events (2024-11-05 spec). Client opens a GET on the SSE URL; the server's first event carries the POST endpoint for JSON-RPC requests, and replies stream back through the same SSE connection matched by JSON-RPC id. Handles concurrent calls through the existing `mu` serialization; pending replies route through a condition-variable-backed map.
- Auto-upgrade from `http` to `sse` when the configured URL's path ends in `/sse`. Keeps callers that hard-code `transport: "http"` against SSE-hosted MCP servers (e.g. Houston MCP) working without config changes.
- `AGT_OPENAI_BASE_URL` env var: when set, overrides the hardcoded OpenAI chat-completions URL. Useful for pointing at OpenAI-compatible self-hosted endpoints (vLLM, Ollama, litellm-proxy) and for tests with no API keys.
- Test fixtures `tests/fixtures/fake_mcp_http.py`, `tests/fixtures/fake_mcp_sse.py`, and `tests/fixtures/fake_openai_llm.py`: minimal HTTP-based fakes for the two new MCP transports and for the OpenAI LLM path. Each prints its ephemeral port on stdout so tests pick it up without shell juggling.
- `tests/test_llm_fake.cpp`: exercises `agt::Llm::complete` end-to-end without real provider credentials via `AGT_OPENAI_BASE_URL`.

### Changed
- `McpTransport` enum gains an `sse` member. Callers that switched on it exhaustively (with `-Wswitch`) need an `sse` branch — `parse_transport` / `transport_name` on the downstream side updated accordingly.

## [0.5.0] - 2026-04-18

### Added
- `make_pg_session(dsn, session_id)`: Postgres-backed conversation persistence alongside the existing `make_sqlite_session`. Same interface; pick the backend that fits your deployment. Runs libpqxx 7.10 under the hood; `find_package(PostgreSQL)` + `libpqxx-dev` on the build host.
- `tests/test_pg_session.cpp`: 7 doctest cases mirroring the SqliteSession suite. Gated on `PG_TEST_DSN`.

## [0.4.0] - 2026-04-17

### Added
- `mcp_config::headers`: list of `(name, value)` pairs sent on every HTTP MCP request. Unblocks talking to authenticated remote MCP servers (e.g. GitHub's hosted `https://api.githubcopilot.com/mcp/`, which requires `Authorization: Bearer ...`).
- Unit-test coverage for the runner loop, MCP client, and session edge cases.

### Fixed
- MCP tool errors no longer abort the runner. `mcp_tool::execute` now catches exceptions thrown by the transport or server and returns them as `{"error": ...}` tool results, so the agent loop keeps running and the model can recover in-conversation.
- JSON-RPC transactions over MCP stdio are now serialised per-connection, fixing a crash under parallel tool-call runs.

## [0.3.0] - 2026-04-11

### Added
- Per-model thinking-support metadata so callers can query whether a given model
  variant exposes reasoning output.
- `agt/version.hpp` header exposing `AGT_VERSION_MAJOR`, `AGT_VERSION_MINOR`,
  `AGT_VERSION_PATCH` and `AGT_VERSION_STRING` for downstream consumers.

### Changed
- Build now requires GCC 15 for full C++23 `<print>` / `std::println` support;
  CI updated accordingly.
- Shared library now carries `VERSION` and `SOVERSION` properties, producing a
  proper `libagt.so` -> `libagt.so.0` -> `libagt.so.0.3.0` symlink chain.
- `provider_to_string` and `provider_from_string` are now header-inlined,
  removing those definitions from the out-of-line ABI surface.
- `Http` implementation moved out of its public header into a source file.

### Fixed
- Gemini thinking mode is now handled correctly.
- Haiku thinking flag is emitted only when applicable, and empty tool-call
  arrays in assistant messages are guarded against.
- Dependency fetches stabilized: pinned `nlohmann_json_VERSION` for the
  json-schema-validator build, and fixed the CPM URL used for json-schema-validator.

[Unreleased]: https://github.com/0x9dhcf/agt/compare/v0.3.0...HEAD
[0.3.0]: https://github.com/0x9dhcf/agt/releases/tag/v0.3.0
