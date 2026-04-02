#include "llm_openai.hpp"

using json = nlohmann::json;

namespace agt {

std::string llm_openai::url() const {
  return "https://api.openai.com/v1/chat/completions";
}

std::vector<std::string> llm_openai::headers() const {
  return {"Content-Type: application/json",
          "Authorization: Bearer " + key_};
}

Json llm_openai::build_request(const Json &input) const {
  Json out;
  out["model"] = model_;

  Json msgs = Json::array();

  /* system -> first message with role "system" */
  if (input.contains("system"))
    msgs.push_back({{"role", "system"}, {"content", input["system"]}});

  for (const auto &m : input["messages"]) {
    Json om;
    om["role"] = m["role"];

    if (m.contains("content"))
      om["content"] = m["content"];
    else
      om["content"] = nullptr;

    /* call_id -> tool_call_id */
    if (m.contains("call_id"))
      om["tool_call_id"] = m["call_id"];

    /* calls -> tool_calls with function wrapper */
    if (m.contains("calls")) {
      Json tcs = Json::array();
      for (const auto &tc : m["calls"]) {
        tcs.push_back(
            {{"id", tc["id"]},
             {"type", "function"},
             {"function", {{"name", tc["name"]}, {"arguments", tc["input"]}}}});
      }
      om["tool_calls"] = std::move(tcs);
    }

    msgs.push_back(std::move(om));
  }
  out["messages"] = std::move(msgs);

  /* tools: wrap in {type:"function", function:{...}} */
  if (input.contains("tools")) {
    Json tools = Json::array();
    for (const auto &tool : input["tools"])
      tools.push_back({{"type", "function"}, {"function", tool}});
    out["tools"] = std::move(tools);
  }

  if (input.contains("max_tokens"))
    out["max_completion_tokens"] = input["max_tokens"];
  if (supports_thinking_ && input.contains("thinking_effort"))
    out["reasoning_effort"] = input["thinking_effort"];

  return out;
}

Json llm_openai::parse_response(const Json &output) const {
  /* API error */
  if (output.contains("error")) {
    const auto &err = output["error"];
    throw LlmError(err.value("code", "api_error") + ": " +
                    err.value("message", "unknown error"));
  }

  const auto &message = output["choices"][0]["message"];
  Json resp;

  /* content */
  resp["content"] = message.value("content", Json(nullptr));

  /* tool_calls -> calls */
  if (message.contains("tool_calls")) {
    Json calls = Json::array();
    for (const auto &tc : message["tool_calls"]) {
      const auto &func = tc["function"];
      calls.push_back({{"id", tc["id"]},
                       {"name", func["name"]},
                       {"input", func.value("arguments", "{}")}});
    }
    resp["calls"] = std::move(calls);
  }

  /* stop_reason: stop->end, length->max_tokens, tool_calls->tool_use */
  auto fr = output["choices"][0].value("finish_reason", "");
  if (fr == "stop")
    resp["stop_reason"] = "end";
  else if (fr == "length")
    resp["stop_reason"] = "max_tokens";
  else if (fr == "tool_calls")
    resp["stop_reason"] = "tool_use";
  else
    resp["stop_reason"] = fr;

  /* usage: prompt_tokens->input_tokens, completion_tokens->output_tokens */
  if (output.contains("usage")) {
    const auto &u = output["usage"];
    resp["usage"] = {{"input_tokens", u["prompt_tokens"]},
                     {"output_tokens", u["completion_tokens"]}};
  }

  return resp;
}

Json llm_openai::build_stream_request(const Json &input) const {
  auto out = build_request(input);
  out["stream"] = true;
  out["stream_options"] = {{"include_usage", true}};
  return out;
}

std::string llm_openai::parse_stream_event(const std::string & /*event*/,
                                           const std::string &data,
                                           Json &accum) const {
  auto chunk = Json::parse(data, nullptr, false);
  if (chunk.is_discarded())
    return {};

  // API error in stream.
  if (chunk.contains("error")) {
    auto &err = chunk["error"];
    throw LlmError(err.value("code", "api_error") + ": " +
                    err.value("message", "unknown error"));
  }

  // Initialize accumulator on first call.
  if (!accum.contains("content"))
    accum["content"] = nullptr;

  auto &choices = chunk["choices"];
  if (!choices.is_array() || choices.empty()) {
    // Usage-only chunk (final).
    if (chunk.contains("usage")) {
      auto &u = chunk["usage"];
      accum["usage"] = {{"input_tokens", u["prompt_tokens"]},
                        {"output_tokens", u["completion_tokens"]}};
    }
    return {};
  }

  auto &choice = choices[0];
  auto &delta = choice["delta"];

  // Finish reason.
  if (choice.contains("finish_reason") && !choice["finish_reason"].is_null()) {
    auto fr = choice["finish_reason"].get<std::string>();
    if (fr == "stop")
      accum["stop_reason"] = "end";
    else if (fr == "length")
      accum["stop_reason"] = "max_tokens";
    else if (fr == "tool_calls")
      accum["stop_reason"] = "tool_use";
    else
      accum["stop_reason"] = fr;
  }

  // Usage (when included alongside choices).
  if (chunk.contains("usage") && !chunk["usage"].is_null()) {
    auto &u = chunk["usage"];
    accum["usage"] = {{"input_tokens", u["prompt_tokens"]},
                      {"output_tokens", u["completion_tokens"]}};
  }

  // Text content delta.
  std::string text_delta;
  if (delta.contains("content") && delta["content"].is_string()) {
    text_delta = delta["content"].get<std::string>();
    if (accum["content"].is_null())
      accum["content"] = text_delta;
    else
      accum["content"] = accum["content"].get<std::string>() + text_delta;
  }

  // Tool call deltas: accumulate by index.
  if (delta.contains("tool_calls")) {
    if (!accum.contains("_tool_calls"))
      accum["_tool_calls"] = Json::object();

    for (auto &tc : delta["tool_calls"]) {
      auto idx = std::to_string(tc["index"].get<int>());
      auto &slot = accum["_tool_calls"][idx];

      if (tc.contains("id"))
        slot["id"] = tc["id"];
      if (tc.contains("function")) {
        if (tc["function"].contains("name"))
          slot["name"] = tc["function"]["name"];
        if (tc["function"].contains("arguments")) {
          if (!slot.contains("arguments"))
            slot["arguments"] = "";
          slot["arguments"] =
              slot["arguments"].get<std::string>() +
              tc["function"]["arguments"].get<std::string>();
        }
      }
    }
  }

  // When stream ends with tool calls, build canonical calls array.
  if (accum.contains("stop_reason") && accum.contains("_tool_calls")) {
    Json calls = Json::array();
    for (const auto &[_, slot] : accum["_tool_calls"].items()) {
      calls.push_back({{"id", slot["id"]},
                       {"name", slot["name"]},
                       {"input", slot.value("arguments", "{}")}});
    }
    accum["calls"] = std::move(calls);
    accum.erase("_tool_calls");
  }

  return text_delta;
}

} // namespace agt
