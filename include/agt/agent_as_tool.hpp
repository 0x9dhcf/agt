// vim: set ts=2 sw=2 sts=2 et:
#pragma once

#include "runner.hpp"
#include <agt/agent.hpp>
#include <agt/llm.hpp>
#include <agt/tool.hpp>
#include <memory>

namespace agt {

/// Tool adapter that wraps an agent, allowing it to be invoked as a tool
/// by another agent. Delegates execution to a runner loop.
class AgentAsTool : public Tool {
public:
  /// Wraps the given agent so it can be called as a tool.
  explicit AgentAsTool(std::shared_ptr<Agent> a, std::shared_ptr<Llm> l,
                       const RunnerOptions& r = {});

  const char* name() const noexcept override;
  const char* description() const noexcept override;
  /// JSON Schema accepting a single "query" string.
  Json parameters() const override;
  /// Runs the wrapped agent with the given query and returns its output.
  Json execute(const Json& input, void* context = nullptr) override;

private:
  std::shared_ptr<Agent> agent_;
  std::shared_ptr<Llm> llm_;
  RunnerOptions options_;
};

} // namespace agt
