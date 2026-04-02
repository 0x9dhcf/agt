#include <doctest/doctest.h>
#include "llm_openai.cpp"

using json = nlohmann::json;

TEST_SUITE("llm_openai") {

static agt::llm_openai make() { return agt::llm_openai("gpt-4", "fake-key"); }

// ── build_request ─────────────────────────────────────────────

TEST_CASE("build_request: system prompt becomes first system message") {
  auto llm = make();
  json input = {{"system", "be helpful"}, {"messages", json::array()}};
  auto req = llm.build_request(input);

  auto &msgs = req["messages"];
  REQUIRE(msgs.size() >= 1);
  CHECK(msgs[0]["role"] == "system");
  CHECK(msgs[0]["content"] == "be helpful");
}

TEST_CASE("build_request: user/assistant messages pass through") {
  auto llm = make();
  json input = {
      {"messages", json::array({{{"role", "user"}, {"content", "hello"}},
                                 {{"role", "assistant"}, {"content", "hi"}}})}};
  auto req = llm.build_request(input);

  auto &msgs = req["messages"];
  REQUIRE(msgs.size() == 2);
  CHECK(msgs[0]["role"] == "user");
  CHECK(msgs[0]["content"] == "hello");
  CHECK(msgs[1]["role"] == "assistant");
  CHECK(msgs[1]["content"] == "hi");
}

TEST_CASE("build_request: tool calls wrapped in function envelope") {
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

  auto &tc = req["messages"][0]["tool_calls"][0];
  CHECK(tc["id"] == "c1");
  CHECK(tc["type"] == "function");
  CHECK(tc["function"]["name"] == "get_weather");
  CHECK(tc["function"]["arguments"] == R"({"city":"NYC"})");
}

TEST_CASE("build_request: tool results map call_id to tool_call_id") {
  auto llm = make();
  json input = {{"messages", json::array({{{"role", "tool"},
                                            {"call_id", "c1"},
                                            {"content", "sunny"}}})}};
  auto req = llm.build_request(input);

  auto &msg = req["messages"][0];
  CHECK(msg["role"] == "tool");
  CHECK(msg["tool_call_id"] == "c1");
  CHECK(msg["content"] == "sunny");
}

TEST_CASE("build_request: tools array wrapped in function envelope") {
  auto llm = make();
  json input = {{"messages", json::array()},
                {"tools", json::array({{{"name", "get_weather"},
                                         {"description", "Get weather"},
                                         {"parameters", {{"type", "object"}}}}})}};
  auto req = llm.build_request(input);

  auto &tools = req["tools"];
  REQUIRE(tools.size() == 1);
  CHECK(tools[0]["type"] == "function");
  CHECK(tools[0]["function"]["name"] == "get_weather");
}

TEST_CASE("build_request: max_tokens passes through") {
  auto llm = make();
  json input = {{"messages", json::array()}, {"max_tokens", 1000}};
  auto req = llm.build_request(input);
  CHECK(req["max_completion_tokens"] == 1000);
}

TEST_CASE("build_request: thinking_effort -> reasoning_effort") {
  auto llm = make();
  json input = {{"messages", json::array()}, {"thinking_effort", "high"}};
  auto req = llm.build_request(input);
  CHECK(req["reasoning_effort"] == "high");
  CHECK_FALSE(req.contains("temperature"));
}

TEST_CASE("build_request: model is set") {
  auto llm = make();
  json input = {{"messages", json::array()}};
  auto req = llm.build_request(input);
  CHECK(req["model"] == "gpt-4");
}

TEST_CASE("build_request: null content preserved") {
  auto llm = make();
  json input = {{"messages", json::array({{{"role", "assistant"}}})}};
  auto req = llm.build_request(input);
  CHECK(req["messages"][0]["content"].is_null());
}

// ── parse_response ────────────────────────────────────────────

TEST_CASE("parse_response: content extracted") {
  auto llm = make();
  json resp = {{"choices", json::array({{{"message", {{"content", "hello"}}},
                                          {"finish_reason", "stop"}}})},
               {"usage", {{"prompt_tokens", 10}, {"completion_tokens", 5}}}};
  auto out = llm.parse_response(resp);
  CHECK(out["content"] == "hello");
}

TEST_CASE("parse_response: finish_reason stop -> end") {
  auto llm = make();
  json resp = {{"choices", json::array({{{"message", {{"content", "hi"}}},
                                          {"finish_reason", "stop"}}})},
               {"usage", {{"prompt_tokens", 1}, {"completion_tokens", 1}}}};
  CHECK(llm.parse_response(resp)["stop_reason"] == "end");
}

TEST_CASE("parse_response: finish_reason length -> max_tokens") {
  auto llm = make();
  json resp = {{"choices", json::array({{{"message", {{"content", ""}}},
                                          {"finish_reason", "length"}}})},
               {"usage", {{"prompt_tokens", 1}, {"completion_tokens", 1}}}};
  CHECK(llm.parse_response(resp)["stop_reason"] == "max_tokens");
}

TEST_CASE("parse_response: finish_reason tool_calls -> tool_use") {
  auto llm = make();
  json resp = {
      {"choices",
       json::array(
           {{{"message",
              {{"content", nullptr},
               {"tool_calls",
                json::array({{{"id", "c1"},
                              {"function", {{"name", "f"}, {"arguments", "{}"}}}
                             }})}}},
             {"finish_reason", "tool_calls"}}})},
      {"usage", {{"prompt_tokens", 1}, {"completion_tokens", 1}}}};
  auto out = llm.parse_response(resp);
  CHECK(out["stop_reason"] == "tool_use");
}

TEST_CASE("parse_response: tool_calls -> canonical calls array") {
  auto llm = make();
  json resp = {
      {"choices",
       json::array(
           {{{"message",
              {{"content", nullptr},
               {"tool_calls",
                json::array(
                    {{{"id", "c1"},
                      {"function",
                       {{"name", "get_weather"},
                        {"arguments", R"({"city":"NYC"})"}}}}}
                )}}},
             {"finish_reason", "tool_calls"}}})},
      {"usage", {{"prompt_tokens", 1}, {"completion_tokens", 1}}}};
  auto out = llm.parse_response(resp);

  REQUIRE(out.contains("calls"));
  REQUIRE(out["calls"].size() == 1);
  CHECK(out["calls"][0]["id"] == "c1");
  CHECK(out["calls"][0]["name"] == "get_weather");
  CHECK(out["calls"][0]["input"] == R"({"city":"NYC"})");
}

TEST_CASE("parse_response: usage prompt_tokens -> input_tokens") {
  auto llm = make();
  json resp = {{"choices", json::array({{{"message", {{"content", "hi"}}},
                                          {"finish_reason", "stop"}}})},
               {"usage", {{"prompt_tokens", 42}, {"completion_tokens", 7}}}};
  auto out = llm.parse_response(resp);
  CHECK(out["usage"]["input_tokens"] == 42);
  CHECK(out["usage"]["output_tokens"] == 7);
}

TEST_CASE("parse_response: null content preserved") {
  auto llm = make();
  json resp = {{"choices", json::array({{{"message", {{"content", nullptr}}},
                                          {"finish_reason", "stop"}}})},
               {"usage", {{"prompt_tokens", 1}, {"completion_tokens", 1}}}};
  auto out = llm.parse_response(resp);
  CHECK(out["content"].is_null());
}

TEST_CASE("parse_response: error response throws llm_error") {
  auto llm = make();
  json resp = {{"error", {{"code", "invalid_api_key"},
                           {"message", "bad key"}}}};
  CHECK_THROWS_AS(llm.parse_response(resp), agt::LlmError);
}

// ── streaming ─────────────────────────────────────────────────

TEST_CASE("build_stream_request: adds stream flag and stream_options") {
  auto llm = make();
  json input = {{"messages", json::array({{{"role", "user"}, {"content", "hi"}}})}};
  auto req = llm.build_stream_request(input);

  CHECK(req["stream"] == true);
  CHECK(req["stream_options"]["include_usage"] == true);
  CHECK(req["model"] == "gpt-4");
  CHECK(req["messages"].size() == 1);
}

TEST_CASE("parse_stream_event: text content delta") {
  auto llm = make();
  json accum;
  std::string data = R"({"choices":[{"index":0,"delta":{"content":"hello"}}]})";

  auto delta = llm.parse_stream_event("", data, accum);
  CHECK(delta == "hello");
  CHECK(accum["content"] == "hello");

  // Second chunk appends.
  data = R"({"choices":[{"index":0,"delta":{"content":" world"}}]})";
  delta = llm.parse_stream_event("", data, accum);
  CHECK(delta == " world");
  CHECK(accum["content"] == "hello world");
}

TEST_CASE("parse_stream_event: finish_reason and usage") {
  auto llm = make();
  json accum;

  // Content chunk.
  llm.parse_stream_event("", R"({"choices":[{"index":0,"delta":{"content":"hi"}}]})", accum);

  // Final chunk with finish_reason.
  llm.parse_stream_event("",
    R"({"choices":[{"index":0,"delta":{},"finish_reason":"stop"}]})", accum);
  CHECK(accum["stop_reason"] == "end");

  // Usage chunk (no choices).
  llm.parse_stream_event("",
    R"({"choices":[],"usage":{"prompt_tokens":10,"completion_tokens":5}})", accum);
  CHECK(accum["usage"]["input_tokens"] == 10);
  CHECK(accum["usage"]["output_tokens"] == 5);
}

TEST_CASE("parse_stream_event: tool call accumulation") {
  auto llm = make();
  json accum;

  // First chunk: tool call start.
  llm.parse_stream_event("",
    R"({"choices":[{"index":0,"delta":{"tool_calls":[{"index":0,"id":"c1","function":{"name":"get_weather","arguments":""}}]}}]})",
    accum);

  // Second chunk: arguments fragment.
  llm.parse_stream_event("",
    R"({"choices":[{"index":0,"delta":{"tool_calls":[{"index":0,"function":{"arguments":"{\"city\":"}}]}}]})",
    accum);

  // Third chunk: arguments fragment.
  llm.parse_stream_event("",
    R"({"choices":[{"index":0,"delta":{"tool_calls":[{"index":0,"function":{"arguments":"\"NYC\"}"}}]}}]})",
    accum);

  // Final chunk: finish_reason.
  llm.parse_stream_event("",
    R"({"choices":[{"index":0,"delta":{},"finish_reason":"tool_calls"}]})",
    accum);

  CHECK(accum["stop_reason"] == "tool_use");
  REQUIRE(accum.contains("calls"));
  REQUIRE(accum["calls"].size() == 1);
  CHECK(accum["calls"][0]["id"] == "c1");
  CHECK(accum["calls"][0]["name"] == "get_weather");
  CHECK(accum["calls"][0]["input"] == R"({"city":"NYC"})");
}

} // TEST_SUITE
