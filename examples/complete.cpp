#include <agt/llm.hpp>
#include <iostream>
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
  }
  return "";
}

int main(int argc, char **argv) {
  if (argc < 3) {
    std::cerr << "usage: " << argv[0] << " <provider> <api_key>\n"
              << "  provider: openai | anthropic | gemini\n";
    return 1;
  }

  agt::Provider p;
  try {
    p = parse_provider(argv[1]);
  } catch (const std::exception &e) {
    std::cerr << e.what() << "\n";
    return 1;
  }

  agt::Llm llm(p, default_model(p), argv[2]);
  agt::Json req = {{"system", "You are a helpful assistant. Be brief."},
                   {"messages", agt::Json::array()}};

  std::string line;
  std::cout << "[" << argv[1] << "] > " << std::flush;

  while (std::getline(std::cin, line)) {
    req["messages"].push_back({{"role", "user"}, {"content", line}});

    try {
      auto res = llm.complete(req);

      auto content = res.value("content", "");
      if (!content.empty())
        std::cout << content << "\n\n";

      req["messages"].push_back({{"role", "assistant"}, {"content", content}});
    } catch (const agt::LlmError &e) {
      std::cerr << "error: " << e.what() << "\n";
      return 1;
    }

    std::cout << "[" << argv[1] << "] > " << std::flush;
  }

  return 0;
}
