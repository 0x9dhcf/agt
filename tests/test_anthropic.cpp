#include <doctest/doctest.h>
#include "llm_anthropic.cpp"

using json = nlohmann::json;

TEST_SUITE("llm_anthropic") {

static agt::llm_anthropic make(bool thinking = true) {
  return agt::llm_anthropic("claude-3-opus", "fake-key", thinking);
}

// ── build_request ─────────────────────────────────────────────

TEST_CASE("build_request: max_tokens defaults to 4096 when absent") {
  auto llm = make();
  json input = {{"messages", json::array()}};
  auto req = llm.build_request(input);
  CHECK(req["max_tokens"] == 4096);
}

TEST_CASE("build_request: max_tokens passes through when present") {
  auto llm = make();
  json input = {{"messages", json::array()}, {"max_tokens", 1024}};
  auto req = llm.build_request(input);
  CHECK(req["max_tokens"] == 1024);
}

TEST_CASE("build_request: system is top-level field") {
  auto llm = make();
  json input = {{"system", "be helpful"}, {"messages", json::array()}};
  auto req = llm.build_request(input);

  CHECK(req["system"] == "be helpful");
  // system should NOT appear in messages
  for (auto &m : req["messages"])
    CHECK(m["role"] != "system");
}

TEST_CASE("build_request: tool result -> user message with tool_result block") {
  auto llm = make();
  json input = {{"messages", json::array({{{"role", "tool"},
                                            {"call_id", "tu_1"},
                                            {"content", "sunny"}}})}};
  auto req = llm.build_request(input);

  auto &msg = req["messages"][0];
  CHECK(msg["role"] == "user");
  REQUIRE(msg["content"].is_array());
  auto &block = msg["content"][0];
  CHECK(block["type"] == "tool_result");
  CHECK(block["tool_use_id"] == "tu_1");
  CHECK(block["content"] == "sunny");
}

TEST_CASE("build_request: assistant with calls -> content array with text + "
          "tool_use blocks") {
  auto llm = make();
  json input = {
      {"messages",
       json::array({{{"role", "assistant"},
                      {"content", "calling tool"},
                      {"calls",
                       json::array({{{"id", "tu_1"},
                                     {"name", "get_weather"},
                                     {"input", R"({"city":"NYC"})"}}})}}})}};
  auto req = llm.build_request(input);

  auto &content = req["messages"][0]["content"];
  REQUIRE(content.is_array());
  REQUIRE(content.size() == 2);

  CHECK(content[0]["type"] == "text");
  CHECK(content[0]["text"] == "calling tool");

  CHECK(content[1]["type"] == "tool_use");
  CHECK(content[1]["id"] == "tu_1");
  CHECK(content[1]["name"] == "get_weather");
  CHECK(content[1]["input"]["city"] == "NYC"); // parsed from string to object
}

TEST_CASE("build_request: tools parameters -> input_schema") {
  auto llm = make();
  json input = {{"messages", json::array()},
                {"tools", json::array({{{"name", "get_weather"},
                                         {"description", "Get weather"},
                                         {"parameters", {{"type", "object"}}}}})}};
  auto req = llm.build_request(input);

  auto &tool = req["tools"][0];
  CHECK(tool["name"] == "get_weather");
  CHECK(tool.contains("input_schema"));
  CHECK(tool["input_schema"]["type"] == "object");
  CHECK_FALSE(tool.contains("parameters"));
}

TEST_CASE("build_request: thinking_effort -> output_config.effort when supported") {
  auto llm = make(true);
  json input = {{"messages", json::array()}, {"thinking_effort", "high"}};
  auto req = llm.build_request(input);
  CHECK(req["output_config"]["effort"] == "high");
}

TEST_CASE("build_request: thinking_effort none omits output_config") {
  auto llm = make(true);
  json input = {{"messages", json::array()}, {"thinking_effort", "none"}};
  auto req = llm.build_request(input);
  CHECK_FALSE(req.contains("output_config"));
}

TEST_CASE("build_request: thinking_effort suppressed when not supported") {
  auto llm = make(false);
  json input = {{"messages", json::array()}, {"thinking_effort", "high"}};
  auto req = llm.build_request(input);
  CHECK_FALSE(req.contains("output_config"));
}

TEST_CASE("build_request: model is set") {
  auto llm = make();
  json input = {{"messages", json::array()}};
  auto req = llm.build_request(input);
  CHECK(req["model"] == "claude-3-opus");
}

// ── parse_response ────────────────────────────────────────────

TEST_CASE("parse_response: text content extracted from blocks") {
  auto llm = make();
  json resp = {{"content", json::array({{{"type", "text"}, {"text", "hello"}}})},
               {"stop_reason", "end_turn"},
               {"usage", {{"input_tokens", 10}, {"output_tokens", 5}}}};
  auto out = llm.parse_response(resp);
  CHECK(out["content"] == "hello");
}

TEST_CASE("parse_response: tool_use blocks -> canonical calls") {
  auto llm = make();
  json resp = {
      {"content",
       json::array({{{"type", "tool_use"},
                      {"id", "tu_1"},
                      {"name", "get_weather"},
                      {"input", {{"city", "NYC"}}}}})},
      {"stop_reason", "tool_use"},
      {"usage", {{"input_tokens", 1}, {"output_tokens", 1}}}};
  auto out = llm.parse_response(resp);

  REQUIRE(out.contains("calls"));
  REQUIRE(out["calls"].size() == 1);
  CHECK(out["calls"][0]["id"] == "tu_1");
  CHECK(out["calls"][0]["name"] == "get_weather");
  // input should be stringified
  auto parsed = json::parse(out["calls"][0]["input"].get<std::string>());
  CHECK(parsed["city"] == "NYC");
}

TEST_CASE("parse_response: stop_reason end_turn -> end") {
  auto llm = make();
  json resp = {{"content", json::array({{{"type", "text"}, {"text", "hi"}}})},
               {"stop_reason", "end_turn"},
               {"usage", {{"input_tokens", 1}, {"output_tokens", 1}}}};
  CHECK(llm.parse_response(resp)["stop_reason"] == "end");
}

TEST_CASE("parse_response: stop_reason max_tokens preserved") {
  auto llm = make();
  json resp = {{"content", json::array({{{"type", "text"}, {"text", ""}}})},
               {"stop_reason", "max_tokens"},
               {"usage", {{"input_tokens", 1}, {"output_tokens", 1}}}};
  CHECK(llm.parse_response(resp)["stop_reason"] == "max_tokens");
}

TEST_CASE("parse_response: stop_reason tool_use preserved") {
  auto llm = make();
  json resp = {
      {"content",
       json::array({{{"type", "tool_use"},
                      {"id", "tu_1"},
                      {"name", "f"},
                      {"input", json::object()}}})},
      {"stop_reason", "tool_use"},
      {"usage", {{"input_tokens", 1}, {"output_tokens", 1}}}};
  CHECK(llm.parse_response(resp)["stop_reason"] == "tool_use");
}

TEST_CASE("parse_response: usage passes through unchanged") {
  auto llm = make();
  json resp = {{"content", json::array({{{"type", "text"}, {"text", "hi"}}})},
               {"stop_reason", "end_turn"},
               {"usage", {{"input_tokens", 42}, {"output_tokens", 7}}}};
  auto out = llm.parse_response(resp);
  CHECK(out["usage"]["input_tokens"] == 42);
  CHECK(out["usage"]["output_tokens"] == 7);
}

TEST_CASE("parse_response: error response throws llm_error") {
  auto llm = make();
  json resp = {{"type", "error"},
               {"error", {{"type", "authentication_error"},
                           {"message", "invalid api key"}}}};
  CHECK_THROWS_AS(llm.parse_response(resp), agt::LlmError);
}

TEST_CASE("parse_response: null content when no text blocks") {
  auto llm = make();
  json resp = {
      {"content",
       json::array({{{"type", "tool_use"},
                      {"id", "tu_1"},
                      {"name", "f"},
                      {"input", json::object()}}})},
      {"stop_reason", "tool_use"},
      {"usage", {{"input_tokens", 1}, {"output_tokens", 1}}}};
  auto out = llm.parse_response(resp);
  CHECK(out["content"].is_null());
}

// ── streaming ─────────────────────────────────────────────────

TEST_CASE("build_stream_request: adds stream flag") {
  auto llm = make();
  json input = {{"messages", json::array({{{"role", "user"}, {"content", "hi"}}})}};
  auto req = llm.build_stream_request(input);

  CHECK(req["stream"] == true);
  CHECK(req["model"] == "claude-3-opus");
}

TEST_CASE("parse_stream_event: text delta from content_block_delta") {
  auto llm = make();
  json accum;

  // message_start with usage.
  llm.parse_stream_event("message_start",
    R"({"message":{"usage":{"input_tokens":10,"output_tokens":0}}})", accum);
  CHECK(accum["usage"]["input_tokens"] == 10);

  // content_block_start (text).
  llm.parse_stream_event("content_block_start",
    R"({"content_block":{"type":"text","text":""}})", accum);

  // text deltas.
  auto d1 = llm.parse_stream_event("content_block_delta",
    R"({"delta":{"type":"text_delta","text":"hello"}})", accum);
  CHECK(d1 == "hello");

  auto d2 = llm.parse_stream_event("content_block_delta",
    R"({"delta":{"type":"text_delta","text":" world"}})", accum);
  CHECK(d2 == " world");
  CHECK(accum["content"] == "hello world");

  // message_delta with stop_reason and output usage.
  llm.parse_stream_event("message_delta",
    R"({"delta":{"stop_reason":"end_turn"},"usage":{"output_tokens":5}})", accum);
  CHECK(accum["stop_reason"] == "end");
  CHECK(accum["usage"]["output_tokens"] == 5);
}

TEST_CASE("parse_stream_event: tool use accumulation") {
  auto llm = make();
  json accum;

  llm.parse_stream_event("message_start",
    R"({"message":{"usage":{"input_tokens":5,"output_tokens":0}}})", accum);

  // content_block_start for tool_use.
  llm.parse_stream_event("content_block_start",
    R"({"content_block":{"type":"tool_use","id":"tu_1","name":"get_weather"}})", accum);

  // input_json_delta chunks.
  llm.parse_stream_event("content_block_delta",
    R"({"delta":{"type":"input_json_delta","partial_json":"{\"city\":"}})", accum);
  llm.parse_stream_event("content_block_delta",
    R"({"delta":{"type":"input_json_delta","partial_json":"\"NYC\"}"}})", accum);

  // message_delta: stop_reason.
  llm.parse_stream_event("message_delta",
    R"({"delta":{"stop_reason":"tool_use"},"usage":{"output_tokens":3}})", accum);

  // message_stop: finalize.
  llm.parse_stream_event("message_stop", R"({})", accum);

  CHECK(accum["stop_reason"] == "tool_use");
  REQUIRE(accum.contains("calls"));
  REQUIRE(accum["calls"].size() == 1);
  CHECK(accum["calls"][0]["id"] == "tu_1");
  CHECK(accum["calls"][0]["name"] == "get_weather");
  CHECK(accum["calls"][0]["input"] == R"({"city":"NYC"})");
}

} // TEST_SUITE
