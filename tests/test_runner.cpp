#include <doctest/doctest.h>
#include "llm_impl.hpp" // complete type needed for unique_ptr in llm stubs
#include "runner.cpp"

using json = nlohmann::json;

// ── Linker stubs for llm class (not exercised by unit tests) ──

namespace agt {
Llm::Llm(Provider, const std::string &, const std::string &) {}
Llm::~Llm() noexcept = default;
Llm::Llm(Llm &&) noexcept = default;
Llm &Llm::operator=(Llm &&) noexcept = default;
Json Llm::complete(const Json &) { return {}; }
Json Llm::complete(const Json &, on_token_cb) { return {}; }

// Provider utility stubs (declared in llm.hpp, defined in llm.cpp)
const char *provider_to_string(Provider) noexcept { return "stub"; }
Provider provider_from_string(const std::string &) { return Provider::openai; }
std::vector<ModelInfo> curated_models(Provider) { return {}; }
bool model_supports_thinking(Provider, const std::string &) { return false; }
} // namespace agt

// ── Mock tool ─────────────────────────────────────────────────

class mock_tool : public agt::Tool {
  std::string name_;
  std::string desc_;
  json params_;
  json result_;

public:
  mock_tool(std::string name, std::string desc, json params, json result)
      : name_(std::move(name)), desc_(std::move(desc)),
        params_(std::move(params)), result_(std::move(result)) {}

  const char *name() const noexcept override { return name_.c_str(); }
  const char *description() const noexcept override { return desc_.c_str(); }
  json parameters() const override { return params_; }
  json execute(const json &, void * = nullptr) override { return result_; }
};

static std::shared_ptr<mock_tool>
make_tool(const std::string &name = "test_tool",
          json result = "tool_result",
          json params = {{"type", "object"},
                         {"properties", json::object()},
                         {"required", json::array()}}) {
  return std::make_shared<mock_tool>(name, "A test tool", std::move(params),
                                     std::move(result));
}

// ── get_content ───────────────────────────────────────────────

TEST_SUITE("get_content") {

TEST_CASE("extracts string content") {
  json resp = {{"content", "hello"}};
  CHECK(agt::get_content(resp) == "hello");
}

TEST_CASE("returns empty string for null content") {
  json resp = {{"content", nullptr}};
  CHECK(agt::get_content(resp).empty());
}

TEST_CASE("returns empty string for missing content") {
  json resp = json::object();
  CHECK(agt::get_content(resp).empty());
}

} // TEST_SUITE

// ── build_request ─────────────────────────────────────────────

TEST_SUITE("runner::build_request") {

TEST_CASE("user query is last message") {
  agt::Agent a;
  auto req = agt::build_request(a, {},"hello");

  auto &msgs = req["messages"];
  REQUIRE(!msgs.empty());
  auto &last = msgs.back();
  CHECK(last["role"] == "user");
  CHECK(last["content"] == "hello");
}

TEST_CASE("system set from agent instructions; omitted when empty") {
  agt::Agent a;
  a.instructions = "be helpful";
  auto req = agt::build_request(a, {},"hi");
  CHECK(req["system"] == "be helpful");

  agt::Agent b;
  auto req2 = agt::build_request(b, {},"hi");
  CHECK_FALSE(req2.contains("system"));
}

TEST_CASE("tools array built from agent tools; omitted when no tools") {
  auto t = make_tool("my_tool");
  agt::Agent a;
  a.tools = {t};
  auto req = agt::build_request(a, {},"hi");

  REQUIRE(req.contains("tools"));
  REQUIRE(req["tools"].size() == 1);
  CHECK(req["tools"][0]["name"] == "my_tool");

  agt::Agent b;
  auto req2 = agt::build_request(b, {},"hi");
  CHECK_FALSE(req2.contains("tools"));
}

TEST_CASE("session history prepended before query") {
  auto session = std::make_shared<agt::MemorySession>();
  session->append(json::array({{{"role", "user"}, {"content", "old"}},
                                {{"role", "assistant"}, {"content", "reply"}}}));

  agt::Agent a;
  a.session = session;
  auto req = agt::build_request(a, {},"new");

  auto &msgs = req["messages"];
  REQUIRE(msgs.size() == 3);
  CHECK(msgs[0]["content"] == "old");
  CHECK(msgs[1]["content"] == "reply");
  CHECK(msgs[2]["content"] == "new");
}

TEST_CASE("max_tokens and thinking_effort from runner_options") {
  agt::Agent a;
  agt::RunnerOptions opts;
  opts.max_tokens = 2048;
  opts.thinking_effort = "high";
  auto req = agt::build_request(a, opts, "hi");

  CHECK(req["max_tokens"] == 2048);
  CHECK(req["thinking_effort"] == "high");
}

} // TEST_SUITE

// ── execute_tool_calls ────────────────────────────────────────

TEST_SUITE("execute_tool_calls") {

TEST_CASE("known tool: executes and appends tool message with result") {
  auto t = make_tool("my_tool", "result_value");
  std::unordered_map<std::string, std::shared_ptr<agt::Tool>> tool_map;
  tool_map["my_tool"] = t;

  json messages = json::array();
  json resp = {{"calls", json::array({{{"id", "c1"},
                                        {"name", "my_tool"},
                                        {"input", "{}"}}})}};
  agt::RunnerOptions opts;
  agt::RunnerHooks hooks;
  agt::execute_tool_calls(opts, hooks, messages, resp, tool_map);

  REQUIRE(messages.size() == 1);
  CHECK(messages[0]["role"] == "tool");
  CHECK(messages[0]["call_id"] == "c1");
  CHECK(messages[0]["content"] == "\"result_value\"");
}

TEST_CASE("unknown tool: appends tool not found message") {
  std::unordered_map<std::string, std::shared_ptr<agt::Tool>> tool_map;

  json messages = json::array();
  json resp = {{"calls", json::array({{{"id", "c1"},
                                        {"name", "missing_tool"},
                                        {"input", "{}"}}})}};
  agt::RunnerOptions opts;
  agt::RunnerHooks hooks;
  agt::execute_tool_calls(opts, hooks, messages, resp, tool_map);

  REQUIRE(messages.size() == 1);
  CHECK(messages[0]["role"] == "tool");
  auto content = messages[0]["content"].get<std::string>();
  CHECK(content.find("tool not found") != std::string::npos);
}

TEST_CASE("invalid input (fails schema validation): appends validation error") {
  auto params = json{{"type", "object"},
                     {"properties", {{"name", {{"type", "string"}}}}},
                     {"required", json::array({"name"})}};
  auto t = make_tool("strict_tool", "ok", params);
  std::unordered_map<std::string, std::shared_ptr<agt::Tool>> tool_map;
  tool_map["strict_tool"] = t;

  json messages = json::array();
  // Missing required "name" field
  json resp = {{"calls", json::array({{{"id", "c1"},
                                        {"name", "strict_tool"},
                                        {"input", "{}"}}})}};
  agt::RunnerOptions opts;
  agt::RunnerHooks hooks;
  agt::execute_tool_calls(opts, hooks, messages, resp, tool_map);

  REQUIRE(messages.size() == 1);
  auto content = messages[0]["content"].get<std::string>();
  CHECK(content.find("invalid tool input") != std::string::npos);
}

TEST_CASE("on_tool_start returning false: appends tool call denied") {
  auto t = make_tool("my_tool");
  std::unordered_map<std::string, std::shared_ptr<agt::Tool>> tool_map;
  tool_map["my_tool"] = t;

  json messages = json::array();
  json resp = {{"calls", json::array({{{"id", "c1"},
                                        {"name", "my_tool"},
                                        {"input", "{}"}}})}};
  agt::RunnerOptions opts;
  agt::RunnerHooks hooks;
  hooks.on_tool_start = [](const agt::Tool &, const json &) { return false; };
  agt::execute_tool_calls(opts, hooks, messages, resp, tool_map);

  REQUIRE(messages.size() == 1);
  auto content = messages[0]["content"].get<std::string>();
  CHECK(content.find("tool call denied") != std::string::npos);
}

TEST_CASE("on_tool_stop receives correct tool, args, and result") {
  auto t = make_tool("my_tool", "the_result");
  std::unordered_map<std::string, std::shared_ptr<agt::Tool>> tool_map;
  tool_map["my_tool"] = t;

  std::string captured_name;
  json captured_args;
  json captured_result;

  json messages = json::array();
  json resp = {{"calls", json::array({{{"id", "c1"},
                                        {"name", "my_tool"},
                                        {"input", R"({"x":1})"}}})}};
  agt::RunnerOptions opts;
  agt::RunnerHooks hooks;
  hooks.on_tool_stop = [&](const agt::Tool &tool, const json &args,
                           const json &result) {
    captured_name = tool.name();
    captured_args = args;
    captured_result = result;
  };
  agt::execute_tool_calls(opts, hooks, messages, resp, tool_map);

  CHECK(captured_name == "my_tool");
  CHECK(captured_args["x"] == 1);
  CHECK(captured_result == "the_result");
}

} // TEST_SUITE
