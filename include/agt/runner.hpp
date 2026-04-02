#pragma once

#include "tool.hpp"
#include <agt/json.hpp>
#include <agt/llm.hpp>
#include <iostream>
#include <optional>
#include <string>

namespace agt {

class Agent;

struct RunnerHooks {
  on_token_cb on_token = nullptr;
  std::function<void()> on_start = nullptr;
  std::function<void(const Llm& l, const Json& req)> on_llm_start = nullptr;
  std::function<void(const Llm& l, const Json& res)> on_llm_stop = nullptr;
  /// Return false to deny execution; the tool result will report denial.
  std::function<bool(const Tool& t, const Json& args)> on_tool_start = nullptr;
  std::function<void(const Tool& t, const Json& args, const Json& result)> on_tool_stop = nullptr;
  std::function<void()> on_stop = nullptr;
};

struct RunnerOptions {
  unsigned int max_turns = 50;                ///< Max LLM round-trips before stopping.
  int max_tokens = 4096;                      ///< Max output tokens per LLM call.
  void* context = nullptr;                    ///< Opaque pointer forwarded to tool::execute().
  int max_input_tokens = 0;                   ///< 0 = no limit. Compact session when exceeded.
  int compact_keep = 20;                      ///< Messages to keep after compaction.
  std::optional<std::string> thinking_effort; ///< "none" | "low" | "medium" | "high"
};

/// Returns hooks that log every runner event (LLM calls, tool use, etc.)
/// to \p os in a human-readable format.  Useful during development.
RunnerHooks debug_hooks(std::ostream& os = std::cerr);

/// Result of a runner::run() invocation.
struct Response {
  enum Status { ok, error, cancelled };

  Status status;
  std::string content; ///< Final text output from the agent.
  Json messages;       ///< Full message history (JSON array).
  int input_tokens = 0;
  int output_tokens = 0;
};

/// Executes an agentic loop: send query, handle tool calls, repeat until
/// the LLM stops or max_turns is reached.
class Runner {
public:
  Response run(Llm& llm, const Agent& a, const std::string& query, const RunnerOptions& opts = {},
               const RunnerHooks& hooks = {});
};

} // namespace agt
