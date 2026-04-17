#include <deque>
#include <doctest/doctest.h>
#include "llm_impl.hpp" // complete type needed for unique_ptr in llm stubs
#include "runner.cpp"

using json = nlohmann::json;

// ── Scripted Llm stubs ────────────────────────────────────────
// Tests push the sequence of responses they want the fake Llm to return, then
// construct an Llm and invoke Runner::run. Each complete() pops one entry.
// If a response contains an "emit_tokens" array, the streaming overload
// forwards each element to the on_token callback before returning.
namespace {
std::deque<json>& script() {
  static std::deque<json> q;
  return q;
}
json pop_script() {
  auto& q = script();
  if (q.empty())
    return {};
  auto r = std::move(q.front());
  q.pop_front();
  return r;
}
} // namespace

namespace agt {
Llm::Llm(Provider, const std::string &, const std::string &) {}
Llm::~Llm() noexcept = default;
Llm::Llm(Llm &&) noexcept = default;
Llm &Llm::operator=(Llm &&) noexcept = default;
Json Llm::complete(const Json &) { return pop_script(); }
Json Llm::complete(const Json &, on_token_cb on_token) {
  auto r = pop_script();
  if (on_token && r.contains("emit_tokens") && r["emit_tokens"].is_array()) {
    for (const auto& tok : r["emit_tokens"])
      on_token(tok.get<std::string>());
  }
  return r;
}

// Provider utility stubs (declared in llm.hpp, defined in llm.cpp)
std::vector<ModelInfo> curated_models(Provider) { return {}; }
bool model_supports_thinking(Provider, const std::string &) { return false; }
} // namespace agt

// Helper: build an Llm without touching any real provider.
static agt::Llm make_fake_llm() {
  return agt::Llm(agt::Provider::unknown, "fake", "");
}

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

TEST_CASE("tool input with invalid JSON throws (documents current behavior)") {
  auto t = make_tool("my_tool");
  std::unordered_map<std::string, std::shared_ptr<agt::Tool>> tool_map;
  tool_map["my_tool"] = t;

  json messages = json::array();
  json resp = {{"calls", json::array({{{"id", "c1"},
                                        {"name", "my_tool"},
                                        {"input", "not-json"}}})}};
  agt::RunnerOptions opts;
  agt::RunnerHooks hooks;
  CHECK_THROWS(agt::execute_tool_calls(opts, hooks, messages, resp, tool_map));
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

// ── Runner::run ───────────────────────────────────────────────

TEST_SUITE("Runner::run") {

TEST_CASE("single-turn end: returns ok with content and accumulates usage") {
  script().clear();
  script().push_back({{"stop_reason", "end"},
                      {"content", "hello there"},
                      {"usage", {{"input_tokens", 10}, {"output_tokens", 5}}}});

  auto llm = make_fake_llm();
  agt::Agent a;
  agt::Runner r;
  auto res = r.run(llm, a, "hi");

  CHECK(res.status == agt::Response::ok);
  CHECK(res.content == "hello there");
  CHECK(res.input_tokens == 10);
  CHECK(res.output_tokens == 5);
  // messages: user query + assistant reply
  REQUIRE(res.messages.size() == 2);
  CHECK(res.messages[0]["role"] == "user");
  CHECK(res.messages[1]["role"] == "assistant");
}

TEST_CASE("max_tokens stop_reason: returns cancelled with token-limit message") {
  script().clear();
  script().push_back({{"stop_reason", "max_tokens"}, {"content", "partial"}});

  auto llm = make_fake_llm();
  agt::Agent a;
  agt::Runner r;
  auto res = r.run(llm, a, "hi");

  CHECK(res.status == agt::Response::cancelled);
  CHECK(res.content == "token limit reached");
}

TEST_CASE("unknown stop_reason: returns error") {
  script().clear();
  script().push_back({{"stop_reason", "bogus"}, {"content", ""}});

  auto llm = make_fake_llm();
  agt::Agent a;
  agt::Runner r;
  auto res = r.run(llm, a, "hi");

  CHECK(res.status == agt::Response::error);
  CHECK(res.content.find("bogus") != std::string::npos);
}

TEST_CASE("tool_use loop: runs tool, sends results, terminates on end") {
  auto t = make_tool("my_tool", "tool_ok");
  script().clear();
  script().push_back({{"stop_reason", "tool_use"},
                      {"content", ""},
                      {"calls", json::array({{{"id", "c1"},
                                               {"name", "my_tool"},
                                               {"input", "{}"}}})}});
  script().push_back({{"stop_reason", "end"}, {"content", "all done"}});

  auto llm = make_fake_llm();
  agt::Agent a;
  a.tools = {t};
  agt::Runner r;
  auto res = r.run(llm, a, "hi");

  CHECK(res.status == agt::Response::ok);
  CHECK(res.content == "all done");
  // user + assistant(with calls) + tool result + assistant(final)
  REQUIRE(res.messages.size() == 4);
  CHECK(res.messages[1]["calls"][0]["id"] == "c1");
  CHECK(res.messages[2]["role"] == "tool");
  CHECK(res.messages[2]["call_id"] == "c1");
}

TEST_CASE("max_turns reached: returns cancelled") {
  auto t = make_tool("my_tool", "ok");
  script().clear();
  // Endless tool_use loop — runner should bail after max_turns.
  for (int i = 0; i < 5; ++i)
    script().push_back({{"stop_reason", "tool_use"},
                        {"content", ""},
                        {"calls", json::array({{{"id", "c"+std::to_string(i)},
                                                 {"name", "my_tool"},
                                                 {"input", "{}"}}})}});

  auto llm = make_fake_llm();
  agt::Agent a;
  a.tools = {t};
  agt::RunnerOptions opts;
  opts.max_turns = 2;
  agt::Runner r;
  auto res = r.run(llm, a, "hi", opts);

  CHECK(res.status == agt::Response::cancelled);
  CHECK(res.content == "max turns reached");
}

TEST_CASE("usage accumulates across tool_use turns") {
  auto t = make_tool("my_tool", "ok");
  script().clear();
  script().push_back({{"stop_reason", "tool_use"},
                      {"content", ""},
                      {"calls", json::array({{{"id", "c1"},
                                               {"name", "my_tool"},
                                               {"input", "{}"}}})},
                      {"usage", {{"input_tokens", 3}, {"output_tokens", 7}}}});
  script().push_back({{"stop_reason", "end"},
                      {"content", "done"},
                      {"usage", {{"input_tokens", 4}, {"output_tokens", 6}}}});

  auto llm = make_fake_llm();
  agt::Agent a;
  a.tools = {t};
  agt::Runner r;
  auto res = r.run(llm, a, "hi");

  CHECK(res.input_tokens == 7);
  CHECK(res.output_tokens == 13);
}

TEST_CASE("hooks fire in the expected order") {
  script().clear();
  script().push_back({{"stop_reason", "end"},
                      {"content", "hi"},
                      {"emit_tokens", json::array({"he", "llo"})}});

  std::vector<std::string> events;
  agt::RunnerHooks hooks;
  hooks.on_start = [&] { events.emplace_back("start"); };
  hooks.on_llm_start = [&](const agt::Llm&, const json&) { events.emplace_back("llm_start"); };
  hooks.on_token = [&](const std::string& t) { events.push_back("tok:" + t); };
  hooks.on_llm_stop = [&](const agt::Llm&, const json&) { events.emplace_back("llm_stop"); };
  hooks.on_stop = [&] { events.emplace_back("stop"); };

  auto llm = make_fake_llm();
  agt::Agent a;
  agt::Runner r;
  r.run(llm, a, "hi", {}, hooks);

  REQUIRE(events.size() == 6);
  CHECK(events[0] == "start");
  CHECK(events[1] == "llm_start");
  CHECK(events[2] == "tok:he");
  CHECK(events[3] == "tok:llo");
  CHECK(events[4] == "llm_stop");
  CHECK(events[5] == "stop");
}

TEST_CASE("session is persisted on end via replace()") {
  auto session = std::make_shared<agt::MemorySession>();
  script().clear();
  script().push_back({{"stop_reason", "end"}, {"content", "yo"}});

  auto llm = make_fake_llm();
  agt::Agent a;
  a.session = session;
  agt::Runner r;
  r.run(llm, a, "hello");

  auto msgs = session->messages();
  REQUIRE(msgs.size() == 2);
  CHECK(msgs[0]["role"] == "user");
  CHECK(msgs[0]["content"] == "hello");
  CHECK(msgs[1]["role"] == "assistant");
}

TEST_CASE("session history prepended on subsequent runs") {
  auto session = std::make_shared<agt::MemorySession>();
  session->append(json::array({{{"role", "user"}, {"content", "earlier"}},
                               {{"role", "assistant"}, {"content", "reply"}}}));

  script().clear();
  script().push_back({{"stop_reason", "end"}, {"content", "ok"}});

  auto llm = make_fake_llm();
  agt::Agent a;
  a.session = session;
  agt::Runner r;
  auto res = r.run(llm, a, "now");

  // 2 history + new user + new assistant
  CHECK(res.messages.size() == 4);
  CHECK(session->messages().size() == 4);
}

TEST_CASE("compaction triggers when input_tokens exceed max_input_tokens") {
  auto t = make_tool("my_tool", "ok");
  auto session = std::make_shared<agt::MemorySession>();

  script().clear();
  // Turn 1: tool_use with input_tokens over the limit → compaction happens.
  script().push_back({{"stop_reason", "tool_use"},
                      {"content", ""},
                      {"calls", json::array({{{"id", "c1"},
                                               {"name", "my_tool"},
                                               {"input", "{}"}}})},
                      {"usage", {{"input_tokens", 1000}, {"output_tokens", 1}}}});
  script().push_back({{"stop_reason", "end"}, {"content", "done"}});

  auto llm = make_fake_llm();
  agt::Agent a;
  a.tools = {t};
  a.session = session;
  agt::RunnerOptions opts;
  opts.max_input_tokens = 100;
  opts.compact_keep = 1; // aggressive compaction to observe effect
  agt::Runner r;
  auto res = r.run(llm, a, "hi", opts);

  CHECK(res.status == agt::Response::ok);
  // After compaction to keep=1 the orphaned tool result is skipped, leaving an
  // empty session. Turn 2 then appends just the final assistant reply.
  REQUIRE(res.messages.size() == 1);
  CHECK(res.messages[0]["role"] == "assistant");
  CHECK(res.messages[0]["content"] == "done");
}

} // TEST_SUITE
