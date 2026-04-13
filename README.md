# agt

![C++23](https://img.shields.io/badge/C%2B%2B-23-blue.svg)
![build](https://github.com/0x9dhcf/agt/actions/workflows/ci.yml/badge.svg)
![platform](https://img.shields.io/badge/platform-Linux-informational.svg?logo=linux&logoColor=white)
![license](https://img.shields.io/badge/license-MIT-green.svg)

Provider-agnostic C++ library for building LLM-powered agents. Supports OpenAI, Anthropic, Gemini, and Mistral with tool calling, MCP integration, streaming, and persistent session management.

## Why agt

agt grew out of [chatty](https://github.com/0x9dhcf/chatty), a personal
terminal assistant built for daily use. Building chatty required a lightweight
agentic loop with tool calling, persistent sessions, and MCP support,
without heavy runtime dependencies or Python in the stack. agt is the library
that came out of that need.

It is intentionally minimal: the API is straightforward, the loop is easy to
follow, and it fits naturally into any C++ project that wants to add LLM-powered
agent capabilities without pulling in a large framework.

## Design

agt is built around a single contract: a **canonical request/response schema**
that all providers translate to and from. Every field a caller passes into
`Llm::complete()` has a defined, validated shape; each backend
(`llm_anthropic`, `llm_openai`, `llm_gemini`, `llm_mistral`) is responsible
for translating the canonical request into its provider-native form and
translating the response back. Callers never see provider-specific JSON.

This contract is what makes the rest of the library work uniformly:

- The `Runner` agentic loop operates on the canonical model only — it knows
  how to validate tool inputs, invoke `agt::Tool::execute()`, fire lifecycle
  hooks, accumulate token usage, and persist sessions, regardless of which
  provider sits behind the `Llm`.
- Switching providers is a one-line change at the call site.
- Sessions are portable across providers: the canonical message history
  recorded under Anthropic can be replayed against Gemini.

### Non-goals: provider-native server-side tools

agt deliberately does **not** expose provider-native server-side tools such
as Anthropic's `web_search_20250305`, Gemini's `google_search`, OpenAI's
`web_search_preview` / `code_interpreter` / `file_search`, or Mistral's
Agents-API connectors.

These tools look superficially similar across providers ("the model can
search the web"), but they break the canonical contract in ways that cannot
be papered over without making the contract meaningless:

- **Each provider's request shape is different** — a tool array entry on
  Anthropic and Gemini, a top-level option on OpenAI's Chat Completions, a
  separate endpoint entirely on Mistral. There is no single canonical field
  that can represent the feature uniformly.
- **Per-provider tunables don't generalize** — `max_uses` (Anthropic),
  `search_context_size` (OpenAI), premium tiers (Mistral), result counts
  (Gemini). Hiding these behind a portable knob either drops them or
  invents a lowest-common-denominator that nobody actually wants.
- **Server-side tool invocations have no canonical response shape** — the
  `calls[]` field represents model→client tool calls. A web-search round
  the model performed server-side has nowhere to land in the canonical
  response, which means `Runner` cannot account for it, hooks cannot
  observe it, and sessions cannot replay it faithfully.
- **Endpoint coverage is uneven** — OpenAI's `web_search_preview` and
  Mistral's connectors live on API endpoints (`/v1/responses`,
  `/v1/agents`) that agt's backends do not target. Pretending to support
  the feature there would mean either silent no-ops or runtime errors —
  neither honest.
- **Adding a raw passthrough field would corrode the canonical model** —
  once the schema accepts un-translated, un-validated JSON whose meaning
  depends on the active provider, every future provider-specific feature
  arrives through the same hatch instead of being designed. The library
  stops being canonical and starts being a thin pre-translator with
  asterisks.

The supported alternative is to implement what you need as a regular
`agt::Tool` that runs locally. For web search, that is a tool whose
`execute()` calls a search API (Brave, Tavily, Kagi, DuckDuckGo, an MCP
server) and returns the results — provider-agnostic, observable through the
normal hooks, persisted in sessions, and fully under your control. agt's
`Tool` interface and `MCP` integration exist precisely to cover these
cases without compromising the canonical core.

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
CPMAddPackage("gh:0x9dhcf/agt@main")
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

See [ROADMAP.md](ROADMAP.md) for the full plan.

## License

MIT. See [LICENSE](LICENSE).
