# Roadmap

## v0.1 - Foundation (current)

- [X] Multi-provider LLM support (OpenAI, Anthropic, Gemini)
- [X] Agent loop with configurable max turns
- [X] Tool calling with JSON Schema parameters
- [X] MCP server integration (stdio + HTTP transports)
- [X] In-memory session/conversation history
- [X] JSON schema validation on all I/O

## v0.2 - Observability & Extensibility

- [X] Lifecycle hooks (before/after callbacks on LLM calls and tool execution)
- [X] Agent as tool helper
- [X] Shared state (void *context on runner_options, forwarded to tool::execute)
- [ ] Tracing (structured event emission for debugging and monitoring)
- [X] Streaming responses (token-by-token SSE-based streaming)

## v0.3 - Multi-Agent

- [ ] Multi-agent orchestration (handoffs, agents-as-tools, delegation)
- [ ] Structured output (schema-constrained agent responses)
- [ ] Dynamic instructions (runtime-generated system prompts)

## v0.4 - Production Readiness

- [ ] Guardrails (input/output/tool validation and safety checks)
- [ ] Human-in-the-loop (pause/resume for tool approval workflows)
- [X] Persistent sessions (SQLite-backed durable conversation storage)
- [X] Context window management (compaction/trimming for long conversations)

## v0.5 - Advanced

- [X] Async execution (concurrent tool calls and parallel agents)
- [ ] Built-in tools (web search, file operations, code execution)
- [X] Extended thinking (Anthropic reasoning mode support)
- [ ] Evaluation framework (systematic agent testing and benchmarking)
