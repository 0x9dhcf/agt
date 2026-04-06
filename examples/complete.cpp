#include <agt/llm.hpp>
#include <iostream>
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

  agt::Llm llm(p, default_model(p), argv[2]);
  agt::Json req = {{"system", "You are a helpful assistant. Be brief."},
                   {"messages", agt::Json::array()}};

  std::string line;
  std::print("[{}] > ", argv[1]);

  while (std::getline(std::cin, line)) {
    req["messages"].push_back({{"role", "user"}, {"content", line}});

    try {
      auto res = llm.complete(req);

      auto content = res.value("content", "");
      if (!content.empty())
        std::println("{}\n", content);

      req["messages"].push_back({{"role", "assistant"}, {"content", content}});
    } catch (const agt::LlmError &e) {
      std::println(stderr, "error: {}", e.what());
      return 1;
    }

    std::print("[{}] > ", argv[1]);
  }

  return 0;
}
