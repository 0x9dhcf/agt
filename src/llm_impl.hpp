#pragma once

#include "http.hpp"
#include <functional>
#include <nlohmann/json.hpp>
#include <string>
#include <vector>

namespace agt {

/// Callback receiving text deltas during streaming.
using on_token_cb = std::function<void(const std::string&)>;

// Per-provider strategy: each subclass translates the canonical JSON
// format to/from its provider's native API.
class LlmImpl {
protected:
  std::string model_;
  std::string key_;
  Http http_;

public:
  virtual std::string url() const = 0;
  virtual std::vector<std::string> headers() const = 0;
  // Translate canonical input to provider-specific request body.
  virtual Json build_request(const Json& input) const = 0;
  // Translate provider-specific response to canonical output.
  virtual Json parse_response(const Json& resp) const = 0;

  // Streaming: build the request body with stream flag enabled.
  virtual Json build_stream_request(const Json& input) const = 0;
  // Streaming: URL may differ (e.g. Gemini uses a different endpoint).
  virtual std::string stream_url() const { return url(); }
  // Streaming: parse one SSE event, update accumulator, return text delta (empty if none).
  virtual std::string parse_stream_event(const std::string& event, const std::string& data,
                                         Json& accum) const = 0;

  LlmImpl(const std::string& model, const std::string& key) : model_(model), key_(key) {}
  virtual ~LlmImpl() noexcept = default;

  Json complete(const Json& input, on_token_cb on_token = nullptr) {
    if (!on_token) {
      // Non-streaming path.
      auto body = build_request(input).dump();
      auto resp = http_.post(url(), body, headers());
      return parse_response(resp);
    }

    // Streaming path.
    auto body = build_stream_request(input).dump();
    Json accum;

    http_.post_stream(stream_url(), body, headers(),
                      [&](const std::string& event, const std::string& data) {
                        auto delta = parse_stream_event(event, data, accum);
                        if (!delta.empty())
                          on_token(delta);
                      });

    // Ensure required output fields have defaults.
    if (!accum.contains("stop_reason"))
      accum["stop_reason"] = "end";
    if (!accum.contains("usage"))
      accum["usage"] = {{"input_tokens", 0}, {"output_tokens", 0}};
    if (!accum.contains("content"))
      accum["content"] = nullptr;

    // Clean up internal accumulator state.
    accum.erase("_tc_idx");
    accum.erase("_tool_calls");
    accum.erase("_tool_blocks");
    accum.erase("_current_block");

    return accum;
  }
};

} // namespace agt
