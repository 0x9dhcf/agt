# agt

![C++23](https://img.shields.io/badge/C%2B%2B-23-blue.svg)
![build](https://github.com/0x9dhcf/agt/actions/workflows/ci.yml/badge.svg)
![license](https://img.shields.io/badge/license-MIT-green.svg)

Provider-agnostic C++ library for building LLM-powered agents. Supports OpenAI, Anthropic, and Gemini with tool calling, MCP integration, streaming, and persistent session management.

## Dependencies

- C++23 compiler
- CMake 3.21+
- libcurl
- SQLite3

Third-party libraries (fetched automatically via [CPM](https://github.com/cpm-cmake/CPM.cmake)):
- [nlohmann/json](https://github.com/nlohmann/json) 3.11.3
- [json-schema-validator](https://github.com/pboettch/json-schema-validator) 2.4.0
- [doctest](https://github.com/doctest/doctest) 2.4.11 (tests only)

## Build

```sh
cmake --preset debug
cmake --build --preset debug
```

For an optimized build (with LTO and `-march=native`):

```sh
cmake --preset release
cmake --build --preset release
```

Debug builds enable AddressSanitizer and UndefinedBehaviorSanitizer.

## Test

```sh
ctest --preset unit
```

Integration tests require provider API keys in the environment:

```sh
export OPENAI_API_KEY=...
export ANTHROPIC_API_KEY=...
export GEMINI_API_KEY=...
ctest --preset integration
```

## Install

```sh
cmake --preset release
cmake --build --preset release
sudo cmake --install build/release
```

## Usage

Link against `agt::agt` in your CMakeLists.txt:

```cmake
find_package(agt REQUIRED)
target_link_libraries(myapp PRIVATE agt::agt)
```

Or fetch directly with CPM:

```cmake
CPMAddPackage("gh:0x9dhcf/agt@0.1.0")
target_link_libraries(myapp PRIVATE agt::agt)
```

### Quick start

```cpp
#include <agt/agent.hpp>
#include <agt/llm.hpp>
#include <agt/runner.hpp>
#include <agt/session.hpp>
#include <agt/tool.hpp>
#include <iostream>

// Define a tool by subclassing agt::Tool.
class get_weather : public agt::Tool {
public:
  const char* name() const noexcept override { return "get_weather"; }
  const char* description() const noexcept override { return "Get current weather for a city."; }
  agt::Json parameters() const override {
    return {{"type", "object"},
            {"properties", {{"city", {{"type", "string"}}}}},
            {"required", {"city"}}};
  }
  agt::Json execute(const agt::Json& input, void*) override {
    return {{"temperature", 22}, {"city", input["city"]}};
  }
};

int main() {
  agt::Llm llm(agt::Provider::anthropic, "claude-sonnet-4-6", getenv("ANTHROPIC_API_KEY"));

  agt::Agent agent;
  agent.instructions = "You are a helpful assistant.";
  agent.tools = {std::make_shared<get_weather>()};
  agent.session = std::make_shared<agt::MemorySession>();

  agt::Runner runner;
  auto resp = runner.run(llm, agent, "What's the weather in Paris?");

  std::cout << resp.content << "\n";
}
```

### Streaming

Pass an `on_token` callback via `RunnerHooks` to receive tokens as they arrive:

```cpp
agt::RunnerHooks hooks;
hooks.on_token = [](const std::string& token) { std::cout << token << std::flush; };

auto resp = runner.run(llm, agent, "Tell me a story.", {}, hooks);
```

### MCP servers

Connect to [MCP](https://modelcontextprotocol.io/) servers to expose external tools:

```cpp
#include <agt/mcp.hpp>

agt::McpServer server({
  .transport = agt::McpTransport::stdio,
  .name = "weather",
  .command = "npx",
  .args = {"-y", "@modelcontextprotocol/server-weather"},
});
server.connect();

// Discovered tools implement agt::Tool and can be added directly to an agent.
agent.tools = server.tools();
```

### Persistent sessions

Use SQLite-backed sessions to persist conversation history across runs:

```cpp
#include <agt/session.hpp>

agent.session = agt::make_sqlite_session("history.db", "session-1");
```

### Agent as tool

Wrap an agent as a tool so it can be invoked by another agent:

```cpp
#include <agt/agent_as_tool.hpp>

auto inner_agent = std::make_shared<agt::Agent>();
inner_agent->instructions = "You are a research assistant.";
inner_agent->tools = { /* ... */ };

auto inner_llm = std::make_shared<agt::Llm>(agt::Provider::openai, "gpt-4o-mini", key);
auto research_tool = std::make_shared<agt::AgentAsTool>(inner_agent, inner_llm);

outer_agent.tools = {research_tool};
```

### Lifecycle hooks

Observe or control the agentic loop with `RunnerHooks`:

```cpp
agt::RunnerHooks hooks;
hooks.on_llm_start = [](const agt::Llm&, const agt::Json& request) { /* log request */ };
hooks.on_llm_stop  = [](const agt::Llm&, const agt::Json& response) { /* log response */ };
hooks.on_tool_start = [](const agt::Tool& t, const agt::Json& input) -> bool {
  // Return false to deny tool execution.
  return true;
};
hooks.on_tool_stop = [](const agt::Tool& t, const agt::Json& input, const agt::Json& output) {};
```

For quick debugging, use the built-in debug hooks:

```cpp
auto hooks = agt::debug_hooks(std::cerr);
```

### Runner options

```cpp
agt::RunnerOptions opts;
opts.max_turns = 50;         // Max LLM round-trips
opts.max_tokens = 4096;      // Max output tokens per call
opts.context = &my_state;    // Opaque pointer forwarded to Tool::execute
opts.max_input_tokens = 0;   // 0 = no limit; triggers session compaction when exceeded
opts.compact_keep = 20;      // Messages retained after compaction
opts.thinking_effort = "high"; // Extended thinking (Anthropic)
```

## API overview

| Header | Description |
|---|---|
| `agt/llm.hpp` | `Llm` client, `Provider` enum, environment helpers |
| `agt/tool.hpp` | `Tool` base class for defining callable tools |
| `agt/runner.hpp` | `Runner` agentic loop, `RunnerOptions`, `RunnerHooks`, `Response` |
| `agt/agent.hpp` | `Agent` struct (instructions, tools, session) |
| `agt/session.hpp` | `Session` base, `MemorySession`, `make_sqlite_session()` |
| `agt/mcp.hpp` | `McpServer` for MCP tool discovery |
| `agt/agent_as_tool.hpp` | `AgentAsTool` wrapper for nested agents |
| `agt/json.hpp` | `Json` type alias for `nlohmann::json` |

## Examples

See the [`examples/`](examples/) directory:

- **complete** -- Direct LLM completion without tools
- **agent** -- Multi-turn agentic loop with custom tools
- **mcp** -- Tool discovery via an MCP server

## Status

This project is in active early development. The v0.1 foundation (multi-provider LLM, tool calling, MCP, sessions) is complete. Current focus areas:

- **v0.2** -- Tracing and structured event emission
- **v0.3** -- Multi-agent orchestration, structured output, dynamic instructions
- **v0.4** -- Guardrails, human-in-the-loop
- **v0.5** -- Built-in tools, evaluation framework

See [ROADMAP.md](ROADMAP.md) for the full plan.

## License

MIT. See [LICENSE](LICENSE).
