// vim: set ts=2 sw=2 sts=2 et:
#pragma once

#include <agt/json.hpp>

namespace agt {

/// Base class for tools that an agent can invoke.
/// Subclass this to expose capabilities (API calls, shell commands, etc.).
class Tool {
public:
  virtual ~Tool() = default;

  /// Unique tool name, used by the LLM to select this tool.
  virtual const char *name() const noexcept = 0;
  /// Human-readable description sent to the LLM.
  virtual const char *description() const noexcept = 0;
  /// JSON Schema describing the expected input object.
  virtual Json parameters() const = 0;
  /// Run the tool with the given input and return the result as JSON.
  /// The context pointer is forwarded from runner_options::context.
  virtual Json execute(const Json &input, void *context = nullptr) = 0;
};

} // namespace agt
