#pragma once

#include <agt/json.hpp>
#include <array>
#include <functional>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

namespace agt {

/// Supported LLM providers.
enum class Provider : std::uint8_t { openai, anthropic, gemini, mistral };

/// Iterable list of all providers.
inline constexpr std::array providers = {Provider::openai, Provider::anthropic, Provider::gemini,
                                         Provider::mistral};

/// Returns the provider name as a C string (e.g. "openai").
const char* provider_to_string(Provider provider) noexcept;

/// Returns the provider from a name (throw if not found).
Provider provider_from_string(const std::string& string);

/// Per-model metadata for curated model lists.
struct ModelInfo {
  std::string id;
  bool supports_thinking = false;
};

struct ProviderConfig {
  std::string key;
  std::vector<ModelInfo> models;
};

/// Returns environment config from environment variables.
std::unordered_map<agt::Provider, ProviderConfig> load_providers_from_env();

/// Returns true for providers with known limitations (e.g. synthetic call IDs).
inline bool is_experimental(Provider provider) noexcept {
  return provider == Provider::gemini;
}

/// Returns a curated list of models known to work well with the canonical schema.
std::vector<ModelInfo> curated_models(Provider provider);

/// Returns true if the given model supports extended thinking for its provider.
/// Returns false for unknown models.
bool model_supports_thinking(Provider provider, const std::string& model);

class LlmImpl;

/// Exception type for all LLM-related errors (network, API, parsing).
class LlmError : public std::runtime_error {
public:
  using std::runtime_error::runtime_error;
};

/// Callback receiving text deltas during streaming.
using on_token_cb = std::function<void(const std::string&)>;

/// Provider-agnostic LLM client. Translates a canonical JSON request/response
/// format to and from each provider's native API.
class Llm {
  std::unique_ptr<LlmImpl> llm_;

public:
  /// Fetches available model IDs from the provider's API.
  // static std::vector<std::string> models(Provider provider, const std::string& key);

  Llm(Provider provider, const std::string& model, const std::string& key);
  ~Llm() noexcept;

  Llm(const Llm&) = delete;
  Llm& operator=(const Llm&) = delete;

  Llm(Llm&&) noexcept;
  Llm& operator=(Llm&&) noexcept;

  /// Sends a completion request and returns the parsed response.
  /// Both input and output are validated against JSON schemas.
  Json complete(const Json& input);

  /// Streaming overload: delivers text deltas to on_token as they arrive,
  /// then returns the full canonical response.
  Json complete(const Json& input, on_token_cb on_token);
};

} // namespace agt
