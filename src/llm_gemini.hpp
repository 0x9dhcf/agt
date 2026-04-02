#pragma once

#include "llm_impl.hpp"

namespace agt {

class llm_gemini : public LlmImpl {
public:
  using LlmImpl::LlmImpl;

  std::string url() const override;
  std::vector<std::string> headers() const override;
  Json build_request(const Json& input) const override;
  Json parse_response(const Json& output) const override;
  Json build_stream_request(const Json& input) const override;
  std::string stream_url() const override;
  std::string parse_stream_event(const std::string& event, const std::string& data,
                                 Json& accum) const override;
};

} // namespace agt
