#include <agt/agent.hpp>
#include <agt/llm.hpp>
#include <agt/mcp.hpp>
#include <agt/runner.hpp>
#include <print>
#include <string>

static agt::Provider parse_provider(const std::string &name) {
  for (auto p : agt::providers)
    if (name == agt::provider_to_string(p))
      return p;
  throw std::runtime_error("unknown provider: " + name);
}

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

  agt::mcp_config weather_cfg{
      .transport = agt::McpTransport::stdio,
      .name      = "weather",
      .command   = "npx",
      .args      = {"-y", "@dangahagan/weather-mcp@latest"},
  };

  agt::McpServer weather(weather_cfg);
  weather.connect();

  agt::Agent a;
  a.instructions = "You are a helpful assistant. Use the available tools "
                   "to answer the user's questions. Be brief and friendly.";
  a.tools        = weather.tools();

  agt::Llm    llm(p, default_model(p), argv[2]);
  agt::Runner runner;

  std::string query = "What's the weather like in Los Angeles?";
  std::println("query: {}\n", query);

  try {
    auto res = runner.run(llm, a, query, {.max_turns = 10});

    std::println("--- messages ---\n{}\n", res.messages.dump(2));

    switch (res.status) {
    case agt::Response::ok:
      std::println("agent: {}", res.content);
      break;
    case agt::Response::error:
      std::println(stderr, "error: {}", res.content);
      return 1;
    case agt::Response::cancelled:
      std::println(stderr, "cancelled: {}", res.content);
      return 1;
    }
  } catch (const std::exception &e) {
    std::println(stderr, "error: {}", e.what());
    return 1;
  }

  return 0;
}
