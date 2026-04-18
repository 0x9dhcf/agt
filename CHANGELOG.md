# Changelog

All notable changes to this project are documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [Unreleased]

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
