#include <agt/llm.hpp>
#include <print>

int main() {
  auto env = agt::load_providers_from_env();

  for (auto p : agt::providers) {
    std::println("{}:", agt::provider_to_string(p));

    auto it = env.find(p);
    if (it == env.end()) {
      std::println("  no API key set");
      continue;
    }

    try {
      for (const auto &m : agt::Llm::models(p, it->second.key))
        std::println("  {}", m);
    } catch (const agt::LlmError &e) {
      std::println(stderr, "  error: {}", e.what());
    }
  }

  return 0;
}
