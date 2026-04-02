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
  CHECK(agt::provider_from_string("mistral") == agt::Provider::mistral);
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
  CHECK(std::ranges::find(openai, "gpt-5", &agt::ModelInfo::id) != openai.end());
  CHECK(std::ranges::find(openai, "o3", &agt::ModelInfo::id) != openai.end());

  auto anthropic = agt::curated_models(agt::Provider::anthropic);
  CHECK(std::ranges::find(anthropic, "claude-sonnet-4-6", &agt::ModelInfo::id) != anthropic.end());

  auto gemini = agt::curated_models(agt::Provider::gemini);
  CHECK(std::ranges::find(gemini, "gemini-2.5-flash", &agt::ModelInfo::id) != gemini.end());
}

TEST_CASE("curated_models thinking support flags") {
  auto openai = agt::curated_models(agt::Provider::openai);
  auto o3 = std::ranges::find(openai, "o3", &agt::ModelInfo::id);
  REQUIRE(o3 != openai.end());
  CHECK(o3->supports_thinking);

  auto gpt5 = std::ranges::find(openai, "gpt-5", &agt::ModelInfo::id);
  REQUIRE(gpt5 != openai.end());
  CHECK_FALSE(gpt5->supports_thinking);

  for (const auto& m : agt::curated_models(agt::Provider::mistral))
    CHECK_FALSE(m.supports_thinking);
}

TEST_CASE("model_supports_thinking for known models") {
  CHECK(agt::model_supports_thinking(agt::Provider::openai, "o3"));
  CHECK(agt::model_supports_thinking(agt::Provider::openai, "o4-mini"));
  CHECK_FALSE(agt::model_supports_thinking(agt::Provider::openai, "gpt-5"));
  CHECK(agt::model_supports_thinking(agt::Provider::anthropic, "claude-sonnet-4-6"));
  CHECK(agt::model_supports_thinking(agt::Provider::gemini, "gemini-2.5-flash"));
  CHECK_FALSE(agt::model_supports_thinking(agt::Provider::mistral, "mistral-large-latest"));
}

TEST_CASE("model_supports_thinking defaults to false for unknown models") {
  CHECK_FALSE(agt::model_supports_thinking(agt::Provider::openai, "unknown-model"));
  CHECK_FALSE(agt::model_supports_thinking(agt::Provider::anthropic, "unknown-model"));
}

} // TEST_SUITE
