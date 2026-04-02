#include <doctest/doctest.h>
#include <agt/llm.hpp>
#include <string>

TEST_SUITE("provider utilities") {

TEST_CASE("provider_to_string round-trips for all providers") {
  for (auto p : agt::providers) {
    auto s = agt::provider_to_string(p);
    CHECK(agt::provider_from_string(s) == p);
  }
}

TEST_CASE("provider_from_string for valid strings") {
  CHECK(agt::provider_from_string("openai") == agt::Provider::openai);
  CHECK(agt::provider_from_string("anthropic") == agt::Provider::anthropic);
  CHECK(agt::provider_from_string("gemini") == agt::Provider::gemini);
}

TEST_CASE("provider_from_string throws on unknown") {
  CHECK_THROWS_AS(agt::provider_from_string("unknown"), std::runtime_error);
  CHECK_THROWS_AS(agt::provider_from_string(""), std::runtime_error);
}


TEST_CASE("is_experimental only true for gemini") {
  CHECK_FALSE(agt::is_experimental(agt::Provider::openai));
  CHECK_FALSE(agt::is_experimental(agt::Provider::anthropic));
  CHECK(agt::is_experimental(agt::Provider::gemini));
}

TEST_CASE("curated_models returns non-empty for all providers") {
  for (auto p : agt::providers) {
    auto models = agt::curated_models(p);
    CHECK_FALSE(models.empty());
  }
}

TEST_CASE("curated_models contains known model names") {
  auto openai = agt::curated_models(agt::Provider::openai);
  CHECK(std::ranges::find(openai, "gpt-5") != openai.end());
  CHECK(std::ranges::find(openai, "o3") != openai.end());

  auto anthropic = agt::curated_models(agt::Provider::anthropic);
  CHECK(std::ranges::find(anthropic, "claude-sonnet-4-6") != anthropic.end());

  auto gemini = agt::curated_models(agt::Provider::gemini);
  CHECK(std::ranges::find(gemini, "gemini-2.5-flash") != gemini.end());
}

} // TEST_SUITE
