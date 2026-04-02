#include <doctest/doctest.h>
#include "llm_gemini.cpp"

using json = nlohmann::json;

// Pull in the file-static coerce_enums for direct testing.
// It's already compiled into this TU via the #include above.

TEST_SUITE("coerce_enums") {

TEST_CASE("numeric enum values coerced to strings, type changed") {
  json schema = {{"type", "integer"}, {"enum", json::array({1, 2, 3})}};
  agt::coerce_enums(schema);

  CHECK(schema["type"] == "string");
  for (auto &v : schema["enum"])
    CHECK(v.is_string());
}

TEST_CASE("string enums unchanged") {
  json schema = {{"type", "string"}, {"enum", json::array({"a", "b"})}};
  agt::coerce_enums(schema);

  CHECK(schema["type"] == "string");
  CHECK(schema["enum"][0] == "a");
  CHECK(schema["enum"][1] == "b");
}

TEST_CASE("recursive through properties and items") {
  json schema = {
      {"type", "object"},
      {"properties",
       {{"status", {{"type", "integer"}, {"enum", json::array({0, 1})}}}}},
      {"items", {{"type", "number"}, {"enum", json::array({1.5, 2.5})}}}};
  agt::coerce_enums(schema);

  CHECK(schema["properties"]["status"]["type"] == "string");
  CHECK(schema["properties"]["status"]["enum"][0].is_string());
  CHECK(schema["items"]["type"] == "string");
  CHECK(schema["items"]["enum"][0].is_string());
}

} // TEST_SUITE

TEST_SUITE("llm_gemini") {

static agt::llm_gemini make() {
  return agt::llm_gemini("gemini-pro", "fake-key");
}

// ── build_request ─────────────────────────────────────────────

TEST_CASE("build_request: system -> systemInstruction") {
  auto llm = make();
  json input = {{"system", "be helpful"}, {"messages", json::array()}};
  auto req = llm.build_request(input);

  CHECK(req["systemInstruction"]["parts"][0]["text"] == "be helpful");
}

TEST_CASE("build_request: assistant role -> model") {
  auto llm = make();
  json input = {
      {"messages",
       json::array({{{"role", "assistant"}, {"content", "hello"}}})}};
  auto req = llm.build_request(input);

  CHECK(req["contents"][0]["role"] == "model");
  CHECK(req["contents"][0]["parts"][0]["text"] == "hello");
}

TEST_CASE("build_request: calls -> functionCall parts with parsed args") {
  auto llm = make();
  json input = {
      {"messages",
       json::array({{{"role", "assistant"},
                      {"content", "calling"},
                      {"calls",
                       json::array({{{"id", "c1"},
                                     {"name", "get_weather"},
                                     {"input", R"({"city":"NYC"})"}}})}}})}};
  auto req = llm.build_request(input);

  auto &parts = req["contents"][0]["parts"];
  REQUIRE(parts.size() == 2);
  CHECK(parts[0]["text"] == "calling");
  CHECK(parts[1]["functionCall"]["name"] == "get_weather");
  CHECK(parts[1]["functionCall"]["args"]["city"] == "NYC");
}

TEST_CASE("build_request: tool result -> functionResponse") {
  auto llm = make();
  json input = {
      {"messages",
       json::array({// assistant with call (for name lookup)
                     {{"role", "assistant"},
                      {"content", ""},
                      {"calls",
                       json::array({{{"id", "c1"},
                                     {"name", "get_weather"},
                                     {"input", "{}"}}})}},
                     // tool result
                     {{"role", "tool"},
                      {"call_id", "c1"},
                      {"content", "sunny"}}})}};
  auto req = llm.build_request(input);

  auto &tool_content = req["contents"][1];
  CHECK(tool_content["role"] == "user");
  auto &fr = tool_content["parts"][0]["functionResponse"];
  CHECK(fr["name"] == "get_weather");
  CHECK(fr["response"]["content"] == "sunny");
}

TEST_CASE("build_request: tools -> functionDeclarations with coerced enums") {
  auto llm = make();
  json input = {
      {"messages", json::array()},
      {"tools",
       json::array({{{"name", "f"},
                      {"description", "desc"},
                      {"parameters",
                       {{"type", "object"},
                        {"properties",
                         {{"status",
                           {{"type", "integer"},
                            {"enum", json::array({0, 1})}}}}}}}}})}};
  auto req = llm.build_request(input);

  auto &decl = req["tools"][0]["functionDeclarations"][0];
  CHECK(decl["name"] == "f");
  // enum values should be coerced to strings
  CHECK(decl["parameters"]["properties"]["status"]["type"] == "string");
  CHECK(decl["parameters"]["properties"]["status"]["enum"][0].is_string());
}

TEST_CASE("build_request: max_tokens -> generationConfig.maxOutputTokens") {
  auto llm = make();
  json input = {{"messages", json::array()}, {"max_tokens", 2048}};
  auto req = llm.build_request(input);
  CHECK(req["generationConfig"]["maxOutputTokens"] == 2048);
}

TEST_CASE("build_request: thinking_effort -> thinkingConfig.thinkingLevel") {
  auto llm = make();
  json input = {{"messages", json::array()}, {"thinking_effort", "high"}};
  auto req = llm.build_request(input);
  CHECK(req["generationConfig"]["thinkingConfig"]["thinkingLevel"] == "high");
  CHECK_FALSE(req.contains("temperature"));
}

TEST_CASE("build_request: thinking_effort none -> thinkingLevel minimal") {
  auto llm = make();
  json input = {{"messages", json::array()}, {"thinking_effort", "none"}};
  auto req = llm.build_request(input);
  CHECK(req["generationConfig"]["thinkingConfig"]["thinkingLevel"] == "minimal");
}

TEST_CASE("build_request: user message -> text parts") {
  auto llm = make();
  json input = {
      {"messages", json::array({{{"role", "user"}, {"content", "hi"}}})}};
  auto req = llm.build_request(input);
  CHECK(req["contents"][0]["role"] == "user");
  CHECK(req["contents"][0]["parts"][0]["text"] == "hi");
}

// ── parse_response ────────────────────────────────────────────

TEST_CASE("parse_response: text parts extracted") {
  auto llm = make();
  json resp = {
      {"candidates",
       json::array(
           {{{"content", {{"parts", json::array({{{"text", "hello"}}})}}},
             {"finishReason", "STOP"}}})},
      {"usageMetadata", {{"promptTokenCount", 10}, {"candidatesTokenCount", 5}}}};
  auto out = llm.parse_response(resp);
  CHECK(out["content"] == "hello");
}

TEST_CASE("parse_response: functionCall parts -> calls with synthetic IDs") {
  auto llm = make();
  json resp = {
      {"candidates",
       json::array(
           {{{"content",
              {{"parts",
                json::array(
                    {{{"functionCall",
                       {{"name", "get_weather"},
                        {"args", {{"city", "NYC"}}}}}}})}}},
             {"finishReason", "STOP"}}})},
      {"usageMetadata",
       {{"promptTokenCount", 1}, {"candidatesTokenCount", 1}}}};
  auto out = llm.parse_response(resp);

  REQUIRE(out.contains("calls"));
  REQUIRE(out["calls"].size() == 1);
  CHECK(out["calls"][0]["id"] == "call_0");
  CHECK(out["calls"][0]["name"] == "get_weather");
  auto parsed = json::parse(out["calls"][0]["input"].get<std::string>());
  CHECK(parsed["city"] == "NYC");
}

TEST_CASE("parse_response: finishReason STOP -> end") {
  auto llm = make();
  json resp = {
      {"candidates",
       json::array(
           {{{"content", {{"parts", json::array({{{"text", "hi"}}})}}},
             {"finishReason", "STOP"}}})},
      {"usageMetadata",
       {{"promptTokenCount", 1}, {"candidatesTokenCount", 1}}}};
  CHECK(llm.parse_response(resp)["stop_reason"] == "end");
}

TEST_CASE("parse_response: finishReason MAX_TOKENS -> max_tokens") {
  auto llm = make();
  json resp = {
      {"candidates",
       json::array(
           {{{"content", {{"parts", json::array({{{"text", ""}}})}}},
             {"finishReason", "MAX_TOKENS"}}})},
      {"usageMetadata",
       {{"promptTokenCount", 1}, {"candidatesTokenCount", 1}}}};
  CHECK(llm.parse_response(resp)["stop_reason"] == "max_tokens");
}

TEST_CASE("parse_response: function calls force stop_reason to tool_use") {
  auto llm = make();
  json resp = {
      {"candidates",
       json::array(
           {{{"content",
              {{"parts",
                json::array(
                    {{{"functionCall",
                       {{"name", "f"}, {"args", json::object()}}}}})}}},
             {"finishReason", "STOP"}}})},
      {"usageMetadata",
       {{"promptTokenCount", 1}, {"candidatesTokenCount", 1}}}};
  auto out = llm.parse_response(resp);
  CHECK(out["stop_reason"] == "tool_use");
}

TEST_CASE("parse_response: usageMetadata mapped correctly") {
  auto llm = make();
  json resp = {
      {"candidates",
       json::array(
           {{{"content", {{"parts", json::array({{{"text", "hi"}}})}}},
             {"finishReason", "STOP"}}})},
      {"usageMetadata",
       {{"promptTokenCount", 42}, {"candidatesTokenCount", 7}}}};
  auto out = llm.parse_response(resp);
  CHECK(out["usage"]["input_tokens"] == 42);
  CHECK(out["usage"]["output_tokens"] == 7);
}

TEST_CASE("parse_response: error response throws llm_error") {
  auto llm = make();
  json resp = {{"error", {{"status", "INVALID_ARGUMENT"},
                           {"message", "bad request"}}}};
  CHECK_THROWS_AS(llm.parse_response(resp), agt::LlmError);
}

// ── streaming ─────────────────────────────────────────────────

TEST_CASE("build_stream_request: same as build_request") {
  auto llm = make();
  json input = {{"messages", json::array({{{"role", "user"}, {"content", "hi"}}})}};
  auto stream_req = llm.build_stream_request(input);
  auto normal_req = llm.build_request(input);
  CHECK(stream_req == normal_req);
}

TEST_CASE("stream_url: uses streamGenerateContent with alt=sse") {
  auto llm = make();
  auto u = llm.stream_url();
  CHECK(u.find("streamGenerateContent") != std::string::npos);
  CHECK(u.find("alt=sse") != std::string::npos);
  CHECK(u.find("gemini-pro") != std::string::npos);
}

TEST_CASE("parse_stream_event: text content delta") {
  auto llm = make();
  json accum;

  auto d1 = llm.parse_stream_event("",
    R"({"candidates":[{"content":{"parts":[{"text":"hello"}]}}]})", accum);
  CHECK(d1 == "hello");
  CHECK(accum["content"] == "hello");

  auto d2 = llm.parse_stream_event("",
    R"({"candidates":[{"content":{"parts":[{"text":" world"}]}}]})", accum);
  CHECK(d2 == " world");
  CHECK(accum["content"] == "hello world");
}

TEST_CASE("parse_stream_event: finish reason and usage") {
  auto llm = make();
  json accum;

  llm.parse_stream_event("",
    R"({"candidates":[{"content":{"parts":[{"text":"hi"}]},"finishReason":"STOP"}],"usageMetadata":{"promptTokenCount":10,"candidatesTokenCount":5}})",
    accum);

  CHECK(accum["stop_reason"] == "end");
  CHECK(accum["usage"]["input_tokens"] == 10);
  CHECK(accum["usage"]["output_tokens"] == 5);
}

TEST_CASE("parse_stream_event: function call") {
  auto llm = make();
  json accum;

  llm.parse_stream_event("",
    R"({"candidates":[{"content":{"parts":[{"functionCall":{"name":"get_weather","args":{"city":"NYC"}}}]},"finishReason":"STOP"}]})",
    accum);

  CHECK(accum["stop_reason"] == "tool_use");
  REQUIRE(accum.contains("calls"));
  REQUIRE(accum["calls"].size() == 1);
  CHECK(accum["calls"][0]["id"] == "call_0");
  CHECK(accum["calls"][0]["name"] == "get_weather");
  auto parsed = json::parse(accum["calls"][0]["input"].get<std::string>());
  CHECK(parsed["city"] == "NYC");
}

} // TEST_SUITE
