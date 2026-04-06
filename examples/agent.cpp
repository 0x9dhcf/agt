#include <agt/agent.hpp>
#include <agt/llm.hpp>
#include <agt/runner.hpp>
#include <agt/session.hpp>
#include <agt/tool.hpp>
#include <memory>
#include <print>
#include <string>

static agt::Provider parse_provider(const std::string &name) {
  for (auto p : agt::providers)
    if (name == agt::provider_to_string(p))
      return p;
  throw std::runtime_error("unknown provider: " + name);
}

static void on_run_start() { std::println("Run starts"); }

static void on_run_stop() { std::println("Run stops"); }

static const char *default_model(agt::Provider p) {
  switch (p) {
  case agt::Provider::openai:
    return "gpt-4o-mini";
  case agt::Provider::anthropic:
    return "claude-sonnet-4-20250514";
  case agt::Provider::gemini:
    return "gemini-2.5-flash";
  case agt::Provider::mistral:
    return "mistral-small-latest";
  }
  return "";
}

class get_weather : public agt::Tool {
public:
  const char *name() const noexcept override { return "get_weather"; }
  const char *description() const noexcept override {
    return "Get the current weather for a given location";
  }
  agt::Json parameters() const override {
    return {
        {"type", "object"},
        {"properties",
         {{"location", {{"type", "string"}, {"description", "City name"}}}}},
        {"required", {"location"}}};
  }
  agt::Json execute(const agt::Json &input, void * = nullptr) override {
    (void)input;
    return {{"temperature", "72F"},
            {"conditions", "sunny"},
            {"wind", "light breeze from the west"}};
  }
};

class get_time : public agt::Tool {
public:
  const char *name() const noexcept override { return "get_time"; }
  const char *description() const noexcept override {
    return "Get the current date and time";
  }
  agt::Json parameters() const override {
    return {{"type", "object"}, {"properties", agt::Json::object()}};
  }
  agt::Json execute(const agt::Json &input, void * = nullptr) override {
    (void)input;
    return "2025-06-15 14:30 UTC";
  }
};

int main(int argc, char **argv) {
  if (argc < 3) {
    std::println(stderr, "usage: {} <provider> <api_key>", argv[0]);
    std::println(stderr, "  provider: openai | anthropic | gemini");
    return 1;
  }

  agt::Provider p;
  try {
    p = parse_provider(argv[1]);
  } catch (const std::exception &e) {
    std::println(stderr, "{}", e.what());
    return 1;
  }

  auto weather = std::make_shared<get_weather>();
  auto time = std::make_shared<get_time>();

  // agt::MemorySession sess;
  auto session = std::make_shared<agt::MemorySession>();

  agt::Agent a;
  a.name = "weather-agent";
  a.description = "An agent that can check weather and time";
  a.instructions = "You are a helpful assistant. Use the available tools "
                   "to answer the user's questions. Be brief and friendly.";
  a.tools = {weather, time};
  a.session = session;

  agt::Llm llm(p, default_model(p), argv[2]);
  agt::Runner runner;

  try {
    std::string q1 = "What's the weather like in Paris?";
    std::println("query 1: {}", q1);
    auto r1 = runner.run(llm, a, q1, {.max_turns = 10},
                         {.on_start = on_run_start, .on_stop = on_run_stop});
    std::println("agent:   {}\n", r1.content);

    std::string q2 = "What about London? And what time is it?";
    std::println("query 2: {}", q2);
    auto r2 = runner.run(llm, a, q2, {.max_turns = 10},
                         {.on_start = on_run_start, .on_stop = on_run_stop});
    std::println("agent:   {}", r2.content);
  } catch (const agt::LlmError &e) {
    std::println(stderr, "llm error: {}", e.what());
    return 1;
  }

  return 0;
}
