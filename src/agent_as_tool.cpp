#include <agt/agent_as_tool.hpp>
#include <agt/runner.hpp>
#include <memory>

namespace agt {

AgentAsTool::AgentAsTool(std::shared_ptr<Agent> a, std::shared_ptr<Llm> l,
                             const RunnerOptions &r)
    : agent_(a), llm_(l), options_(r) {}

const char *AgentAsTool::name() const noexcept { return agent_->name.c_str(); }
const char *AgentAsTool::description() const noexcept { return agent_->description.c_str(); }

Json AgentAsTool::parameters() const {
  return {{"type", "object"},
          {"properties", {{"query", {{"type", "string"}, {"description", "User query"}}}}},
          {"required", {"query"}}};
};

Json AgentAsTool::execute(const Json &input, void *context) {
  auto opts = options_;
  opts.context = context;
  return Runner().run(*llm_, *agent_, input["query"], opts).content;
}

} // namespace agt
