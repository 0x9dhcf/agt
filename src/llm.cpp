#include "http.hpp"
#include "llm_anthropic.hpp"
#include "llm_gemini.hpp"
#include "llm_mistral.hpp"
#include "llm_openai.hpp"
#include "llm_schemas.hpp"
#include <agt/llm.hpp>
#include <memory>
#include <nlohmann/json-schema.hpp>
#include <stdexcept>

namespace agt {

static nlohmann::json_schema::json_validator make_validator(const nlohmann::json& schema) {
  nlohmann::json_schema::json_validator v;
  v.set_root_schema(schema);
  return v;
}

static nlohmann::json_schema::json_validator& output_validator() {
  static auto v = make_validator(schemas::llm_output_schema);
  return v;
}

static const char* provider_environment_key(Provider p) noexcept {
  switch (p) {
  case agt::Provider::openai:
    return "OPENAI_API_KEY";
  case agt::Provider::anthropic:
    return "ANTHROPIC_API_KEY";
  case agt::Provider::gemini:
    return "GEMINI_API_KEY";
  case agt::Provider::mistral:
    return "MISTRAL_API_KEY";
  }
  return "unknown";
}

const char* provider_to_string(Provider p) noexcept {
  switch (p) {
  case Provider::openai:
    return "openai";
  case Provider::anthropic:
    return "anthropic";
  case Provider::gemini:
    return "gemini";
  case Provider::mistral:
    return "mistral";
  }
  return "unknown";
}

Provider provider_from_string(const std::string& s) {
  if ("openai" == s)
    return Provider::openai;
  if ("anthropic" == s)
    return Provider::anthropic;
  if ("gemini" == s)
    return Provider::gemini;
  if ("mistral" == s)
    return Provider::mistral;
  throw std::runtime_error("unknown provider " + s);
}

std::vector<std::string> curated_models(Provider p) {
  switch (p) {
  case Provider::openai:
    return {"gpt-5-nano", "gpt-5-mini", "gpt-5", "gpt-5.1", "gpt-5.2", "o3", "o4-mini"};
  case Provider::anthropic:
    return {"claude-haiku-4-5-20251001", "claude-sonnet-4-6", "claude-opus-4-6"};
  case Provider::gemini:
    return {"gemini-2.5-flash", "gemini-2.5-pro"};
  case Provider::mistral:
    return {"mistral-small-latest", "mistral-medium-latest", "mistral-large-latest"};
  }
  return {};
}

std::unordered_map<agt::Provider, ProviderConfig> load_providers_from_env() {
  std::unordered_map<agt::Provider, ProviderConfig> env;
  for (auto p : agt::providers) {
    auto* env_key = std::getenv(agt::provider_environment_key(p));
    if (env_key)
      env[p] = {.key = env_key, .models = curated_models(p)};
  }
  return env;
}

static nlohmann::json_schema::json_validator& input_validator() {
  static auto v = make_validator(schemas::llm_input_schema);
  return v;
}

Llm::Llm(Provider p, const std::string& model, const std::string& key) {
  switch (p) {
  case Provider::openai:
    llm_ = std::make_unique<llm_openai>(model, key);
    break;
  case Provider::anthropic:
    llm_ = std::make_unique<llm_anthropic>(model, key);
    break;
  case Provider::gemini:
    llm_ = std::make_unique<llm_gemini>(model, key);
    break;
  case Provider::mistral:
    llm_ = std::make_unique<llm_mistral>(model, key);
    break;
  }
}

Llm::~Llm() noexcept = default;
Llm::Llm(Llm&&) noexcept = default;
Llm& Llm::operator=(Llm&&) noexcept = default;

Json Llm::complete(const Json& input) {
  input_validator().validate(input);
  auto output = llm_->complete(input);
  output_validator().validate(output);
  return output;
}

Json Llm::complete(const Json& input, on_token_cb on_token) {
  input_validator().validate(input);
  auto output = llm_->complete(input, std::move(on_token));
  output_validator().validate(output);
  return output;
}

} // namespace agt
