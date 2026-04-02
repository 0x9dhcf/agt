#include "llm_gemini.hpp"

using json = nlohmann::json;

namespace agt {

/*
 * Gemini requires enum values to be strings.
 * Walk a JSON Schema object and coerce any numeric enum entries to strings.
 */
static void coerce_enums(Json &node) {
  if (!node.is_object())
    return;

  if (node.contains("enum") && node["enum"].is_array()) {
    bool coerced = false;
    for (auto &v : node["enum"]) {
      if (v.is_number()) {
        v = std::to_string(v.get<double>());
        coerced = true;
      }
    }
    if (coerced && node.contains("type"))
      node["type"] = "string";
  }

  if (node.contains("properties"))
    for (const auto &[_, p] : node["properties"].items())
      coerce_enums(p);

  if (node.contains("items"))
    coerce_enums(node["items"]);
}

std::string llm_gemini::url() const {
  return "https://generativelanguage.googleapis.com/v1beta/models/" + model_ +
         ":generateContent";
}

std::vector<std::string> llm_gemini::headers() const {
  return {"Content-Type: application/json",
          "x-goog-api-key: " + key_};
}

Json llm_gemini::build_request(const Json &input) const {
  Json out;

  /* systemInstruction */
  if (input.contains("system"))
    out["systemInstruction"] = {
        {"parts", Json::array({{{"text", input["system"]}}})}};

  /* contents */
  Json contents = Json::array();
  const auto &messages = input["messages"];

  for (const auto &m : messages) {
    auto role = m["role"].get<std::string>();

    if (role == "tool") {
      /* Need function name -- look up call_id in prior messages */
      auto call_id = m["call_id"].get<std::string>();
      std::string fn_name = "unknown";

      for (const auto &scan : messages) {
        if (&scan == &m)
          break;
        if (!scan.contains("calls"))
          continue;
        for (const auto &tc : scan["calls"]) {
          if (tc["id"] == call_id)
            fn_name = tc["name"].get<std::string>();
        }
      }

      Json fr = {{"functionResponse",
                  {{"name", fn_name},
                   {"response", {{"content", m["content"]}}}}}};
      contents.push_back(
          {{"role", "user"}, {"parts", Json::array({std::move(fr)})}});
      continue;
    }

    if (role == "assistant") {
      Json parts = Json::array();

      if (m.contains("content") && m["content"].is_string())
        parts.push_back({{"text", m["content"]}});

      if (m.contains("calls")) {
        for (const auto &tc : m["calls"]) {
          /* input is a JSON string -- parse to object for Gemini args */
          Json args =
              Json::parse(tc["input"].get<std::string>(), nullptr, false);
          if (args.is_discarded())
            args = Json::object();
          parts.push_back(
              {{"functionCall",
                {{"name", tc["name"]}, {"args", std::move(args)}}}});
        }
      }

      contents.push_back({{"role", "model"}, {"parts", std::move(parts)}});
      continue;
    }

    /* user message */
    contents.push_back(
        {{"role", "user"},
         {"parts", Json::array({{{"text", m["content"]}}})}});
  }
  out["contents"] = std::move(contents);

  /* tools -> {functionDeclarations: [...]} */
  if (input.contains("tools")) {
    Json decls = Json::array();
    for (const auto &tool : input["tools"]) {
      Json d = {{"name", tool["name"]}, {"description", tool["description"]}};
      if (tool.contains("parameters")) {
        d["parameters"] = tool["parameters"];
        coerce_enums(d["parameters"]);
      }
      decls.push_back(std::move(d));
    }
    out["tools"] = Json::array({{{"functionDeclarations", std::move(decls)}}});
  }

  /* generationConfig */
  Json gc;
  if (input.contains("max_tokens"))
    gc["maxOutputTokens"] = input["max_tokens"];
  if (supports_thinking_ && input.contains("thinking_effort")) {
    auto effort = input["thinking_effort"].get<std::string>();
    auto level = (effort == "none") ? "minimal" : effort;
    gc["thinkingConfig"] = {{"thinkingLevel", level}};
  }
  if (!gc.empty())
    out["generationConfig"] = std::move(gc);

  return out;
}

Json llm_gemini::parse_response(const Json &output) const {
  /* API error: {error:{code,message,status}} */
  if (output.contains("error")) {
    const auto &err = output["error"];
    throw LlmError(err.value("status", "api_error") + ": " +
                    err.value("message", "unknown error"));
  }

  const auto &parts = output["candidates"][0]["content"]["parts"];
  Json resp;

  std::string text;
  Json calls = Json::array();
  bool has_calls = false;
  int tc_idx = 0;

  for (auto &part : parts) {
    if (part.contains("text"))
      text = part["text"].get<std::string>();

    if (part.contains("functionCall")) {
      has_calls = true;
      auto &fc = part["functionCall"];

      /* Gemini doesn't provide call IDs -- generate synthetic ones */
      auto &args = fc["args"];
      calls.push_back({{"id", "call_" + std::to_string(tc_idx++)},
                       {"name", fc["name"]},
                       {"input", args.is_object() ? args.dump() : "{}"}});
    }
  }

  resp["content"] = text.empty() ? Json(nullptr) : Json(text);
  if (has_calls)
    resp["calls"] = std::move(calls);

  /* stop_reason: if tool calls -> tool_use, STOP->end, MAX_TOKENS->max_tokens
   */
  if (has_calls) {
    resp["stop_reason"] = "tool_use";
  } else {
    auto fr = output["candidates"][0].value("finishReason", "");
    if (fr == "STOP")
      resp["stop_reason"] = "end";
    else if (fr == "MAX_TOKENS")
      resp["stop_reason"] = "max_tokens";
    else
      resp["stop_reason"] = fr;
  }

  /* usage: promptTokenCount->input_tokens,
   * candidatesTokenCount->output_tokens */
  if (output.contains("usageMetadata")) {
    auto &u = output["usageMetadata"];
    resp["usage"] = {{"input_tokens", u["promptTokenCount"]},
                     {"output_tokens", u["candidatesTokenCount"]}};
  }

  return resp;
}

Json llm_gemini::build_stream_request(const Json &input) const {
  return build_request(input);
}

std::string llm_gemini::stream_url() const {
  return "https://generativelanguage.googleapis.com/v1beta/models/" + model_ +
         ":streamGenerateContent?alt=sse&key=" + key_;
}

std::string llm_gemini::parse_stream_event(const std::string & /*event*/,
                                           const std::string &data,
                                           Json &accum) const {
  auto chunk = Json::parse(data, nullptr, false);
  if (chunk.is_discarded())
    return {};

  // API error in stream.
  if (chunk.contains("error")) {
    auto &err = chunk["error"];
    throw LlmError(err.value("status", "api_error") + ": " +
                    err.value("message", "unknown error"));
  }

  // Initialize accumulator.
  if (!accum.contains("content"))
    accum["content"] = nullptr;
  if (!accum.contains("_tc_idx"))
    accum["_tc_idx"] = 0;

  std::string text_delta;

  // Extract parts from candidates[0].content.parts.
  if (chunk.contains("candidates") && chunk["candidates"].is_array() &&
      !chunk["candidates"].empty()) {
    auto &candidate = chunk["candidates"][0];

    // Check for error in candidate.
    if (candidate.contains("error")) {
      auto &err = candidate["error"];
      throw LlmError(err.value("status", "api_error") + ": " +
                      err.value("message", "unknown error"));
    }

    if (candidate.contains("content") && candidate["content"].contains("parts")) {
      for (auto &part : candidate["content"]["parts"]) {
        if (part.contains("text")) {
          auto t = part["text"].get<std::string>();
          if (accum["content"].is_null())
            accum["content"] = t;
          else
            accum["content"] = accum["content"].get<std::string>() + t;
          text_delta += t;
        }

        if (part.contains("functionCall")) {
          auto &fc = part["functionCall"];
          if (!accum.contains("calls"))
            accum["calls"] = Json::array();
          auto &args = fc["args"];
          int idx = accum["_tc_idx"].get<int>();
          accum["calls"].push_back(
              {{"id", "call_" + std::to_string(idx)},
               {"name", fc["name"]},
               {"input", args.is_object() ? args.dump() : "{}"}});
          accum["_tc_idx"] = idx + 1;
        }
      }
    }

    // Finish reason.
    if (candidate.contains("finishReason")) {
      auto fr = candidate["finishReason"].get<std::string>();
      if (accum.contains("calls"))
        accum["stop_reason"] = "tool_use";
      else if (fr == "STOP")
        accum["stop_reason"] = "end";
      else if (fr == "MAX_TOKENS")
        accum["stop_reason"] = "max_tokens";
      else
        accum["stop_reason"] = fr;
    }
  }

  // Usage metadata.
  if (chunk.contains("usageMetadata")) {
    auto &u = chunk["usageMetadata"];
    accum["usage"] = {{"input_tokens", u.value("promptTokenCount", 0)},
                      {"output_tokens", u.value("candidatesTokenCount", 0)}};
  }

  // Clean up internal state when we have a final stop_reason.
  if (accum.contains("stop_reason") && accum.contains("_tc_idx"))
    accum.erase("_tc_idx");

  return text_delta;
}

} // namespace agt
