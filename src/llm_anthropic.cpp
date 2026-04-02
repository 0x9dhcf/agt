#include "llm_anthropic.hpp"

namespace agt {

std::string llm_anthropic::url() const {
  return "https://api.anthropic.com/v1/messages";
}

std::vector<std::string> llm_anthropic::headers() const {
  return {"Content-Type: application/json", "x-api-key: " + key_, "anthropic-version: 2023-06-01"};
}

Json llm_anthropic::build_request(const Json& input) const {
  Json out;
  out["model"] = model_;

  /* max_tokens is required for Anthropic -- default 4096 */
  out["max_tokens"] = input.value("max_tokens", 4096);

  if (input.contains("system"))
    out["system"] = input["system"];
  if (input.contains("thinking_effort") && input["thinking_effort"].get<std::string>() != "none")
    out["output_config"] = {{"effort", input["thinking_effort"]}};

  Json msgs = Json::array();

  for (const auto& m : input["messages"]) {
    auto role = m["role"].get<std::string>();

    if (role == "tool") {
      /* {role:"user", content:[{type:"tool_result", tool_use_id, content}]} */
      msgs.push_back({{"role", "user"},
                      {"content", Json::array({{{"type", "tool_result"},
                                                {"tool_use_id", m["call_id"]},
                                                {"content", m["content"]}}})}});
      continue;
    }

    if (role == "assistant" && m.contains("calls")) {
      /* assistant with tool calls -> content array */
      Json blocks = Json::array();

      if (m.contains("content") && m["content"].is_string() && !m["content"].empty())
        blocks.push_back({{"type", "text"}, {"text", m["content"]}});

      for (const auto& tc : m["calls"]) {
        /* input is a JSON string -- parse to object for Anthropic */
        Json in = Json::parse(tc["input"].get<std::string>(), nullptr, false);
        if (in.is_discarded())
          in = Json::object();
        blocks.push_back({{"type", "tool_use"},
                          {"id", tc["id"]},
                          {"name", tc["name"]},
                          {"input", std::move(in)}});
      }

      msgs.push_back({{"role", "assistant"}, {"content", std::move(blocks)}});
      continue;
    }

    /* plain assistant or user message */
    Json om = {{"role", role}};
    if (m.contains("content") && m["content"].is_string() && !m["content"].empty())
      om["content"] = m["content"];
    msgs.push_back(std::move(om));
  }
  out["messages"] = std::move(msgs);

  /* tools: parameters -> input_schema */
  if (input.contains("tools")) {
    Json tools = Json::array();
    for (const auto& tool : input["tools"]) {
      Json t = {{"name", tool["name"]}, {"description", tool["description"]}};
      if (tool.contains("parameters"))
        t["input_schema"] = tool["parameters"];
      tools.push_back(std::move(t));
    }
    out["tools"] = std::move(tools);
  }

  return out;
}

Json llm_anthropic::parse_response(const Json& output) const {
  /* API error: {type:"error", error:{type:"...", message:"..."}} */
  if (output.value("type", "") == "error") {
    const auto& err = output["error"];
    throw LlmError(err.value("type", "api_error") + ": " + err.value("message", "unknown error"));
  }

  Json resp;
  std::string text;
  Json calls = Json::array();
  bool has_calls = false;

  for (const auto& block : output["content"]) {
    auto type = block.value("type", "");

    if (type == "text")
      text = block.value("text", "");
    else if (type == "tool_use") {
      has_calls = true;
      /* stringify input object to JSON string */
      calls.push_back(
          {{"id", block["id"]}, {"name", block["name"]}, {"input", block["input"].dump()}});
    }
  }

  resp["content"] = text.empty() ? Json(nullptr) : Json(text);
  if (has_calls)
    resp["calls"] = std::move(calls);

  /* stop_reason: end_turn->end, max_tokens->max_tokens, tool_use->tool_use */
  auto sr = output.value("stop_reason", "");
  if (sr == "end_turn")
    resp["stop_reason"] = "end";
  else if (sr == "max_tokens")
    resp["stop_reason"] = "max_tokens";
  else if (sr == "tool_use")
    resp["stop_reason"] = "tool_use";
  else
    resp["stop_reason"] = sr;

  /* usage: passthrough (Anthropic already uses input_tokens/output_tokens) */
  if (output.contains("usage")) {
    const auto& u = output["usage"];
    resp["usage"] = {{"input_tokens", u["input_tokens"]}, {"output_tokens", u["output_tokens"]}};
  }

  return resp;
}

Json llm_anthropic::build_stream_request(const Json& input) const {
  auto out = build_request(input);
  out["stream"] = true;
  return out;
}

std::string llm_anthropic::parse_stream_event(const std::string& event, const std::string& data,
                                              Json& accum) const {
  auto chunk = Json::parse(data, nullptr, false);
  if (chunk.is_discarded())
    return {};

  // API error in stream.
  if (event == "error" || (chunk.contains("type") && chunk["type"] == "error")) {
    auto& err = chunk.contains("error") ? chunk["error"] : chunk;
    throw LlmError(err.value("type", "api_error") + ": " + err.value("message", "unknown error"));
  }

  // Initialize accumulator.
  if (!accum.contains("content"))
    accum["content"] = nullptr;

  if (event == "message_start") {
    // Capture input usage from the message object.
    if (chunk.contains("message") && chunk["message"].contains("usage")) {
      auto& u = chunk["message"]["usage"];
      accum["usage"] = {{"input_tokens", u.value("input_tokens", 0)},
                        {"output_tokens", u.value("output_tokens", 0)}};
    }
    return {};
  }

  if (event == "content_block_start") {
    // Track current content block for tool_use accumulation.
    if (chunk.contains("content_block")) {
      auto& block = chunk["content_block"];
      if (block.value("type", "") == "tool_use") {
        if (!accum.contains("_tool_blocks"))
          accum["_tool_blocks"] = Json::array();
        accum["_tool_blocks"].push_back(
            {{"id", block["id"]}, {"name", block["name"]}, {"input_json", ""}});
        accum["_current_block"] = "tool_use";
      } else {
        accum["_current_block"] = "text";
      }
    }
    return {};
  }

  if (event == "content_block_delta") {
    if (!chunk.contains("delta"))
      return {};
    auto& delta = chunk["delta"];
    auto type = delta.value("type", "");

    if (type == "text_delta") {
      auto text = delta.value("text", "");
      if (!text.empty()) {
        if (accum["content"].is_null())
          accum["content"] = text;
        else
          accum["content"] = accum["content"].get<std::string>() + text;
      }
      return text;
    }

    if (type == "input_json_delta") {
      // Accumulate tool input JSON string.
      if (accum.contains("_tool_blocks") && !accum["_tool_blocks"].empty()) {
        auto& last = accum["_tool_blocks"].back();
        last["input_json"] =
            last["input_json"].get<std::string>() + delta.value("partial_json", "");
      }
    }
    return {};
  }

  if (event == "message_delta") {
    // Capture stop_reason and output usage.
    if (chunk.contains("delta")) {
      auto sr = chunk["delta"].value("stop_reason", "");
      if (sr == "end_turn")
        accum["stop_reason"] = "end";
      else if (sr == "max_tokens")
        accum["stop_reason"] = "max_tokens";
      else if (sr == "tool_use")
        accum["stop_reason"] = "tool_use";
      else if (!sr.empty())
        accum["stop_reason"] = sr;
    }
    if (chunk.contains("usage")) {
      auto& u = chunk["usage"];
      if (accum.contains("usage"))
        accum["usage"]["output_tokens"] = u.value("output_tokens", 0);
      else
        accum["usage"] = {{"input_tokens", 0}, {"output_tokens", u.value("output_tokens", 0)}};
    }
    return {};
  }

  if (event == "message_stop") {
    // Finalize tool calls.
    if (accum.contains("_tool_blocks")) {
      Json calls = Json::array();
      for (auto& block : accum["_tool_blocks"]) {
        calls.push_back({{"id", block["id"]},
                         {"name", block["name"]},
                         {"input", block.value("input_json", "{}")}});
      }
      accum["calls"] = std::move(calls);
      accum.erase("_tool_blocks");
    }
    accum.erase("_current_block");
    return {};
  }

  return {};
}

} // namespace agt
