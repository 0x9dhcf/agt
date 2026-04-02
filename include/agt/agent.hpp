#pragma once

#include <memory>
#include <string>
#include <vector>

namespace agt {

class Session;
class Tool;

/// Configuration for an LLM-powered agent.
/// Use tool_ref() for stack-allocated tools; MCP tools are already shared_ptr.
struct Agent {
  std::string name;
  std::string description;
  std::string instructions; ///< System prompt sent to the LLM.
  std::vector<std::shared_ptr<Tool>> tools;
  std::shared_ptr<class Session> session = nullptr; ///< Optional conversation history.
};


} // namespace agt
