#include <agt/llm.hpp>
#include <iostream>

int main() {
  auto env = agt::load_providers_from_env();

  for (auto p : agt::providers) {
    std::cout << agt::provider_to_string(p) << ":\n";

    auto it = env.find(p);
    if (it == env.end()) {
      std::cout << "  no API key set\n";
      continue;
    }

    try {
      for (const auto &m : agt::Llm::models(p, it->second.key))
        std::cout << "  " << m << "\n";
    } catch (const agt::LlmError &e) {
      std::cerr << "  error: " << e.what() << "\n";
    }
  }

  return 0;
}
