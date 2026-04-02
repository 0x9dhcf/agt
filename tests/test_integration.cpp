#include <doctest/doctest.h>
#include <agt/llm.hpp>
#include <cstdlib>
#include <iostream>
#include <string>

using json = agt::Json;

static const char *provider_env_key(agt::Provider p) noexcept {
  switch (p) {
  case agt::Provider::openai:    return "OPENAI_API_KEY";
  case agt::Provider::anthropic: return "ANTHROPIC_API_KEY";
  case agt::Provider::gemini:    return "GEMINI_API_KEY";
  case agt::Provider::mistral:   return "MISTRAL_API_KEY";
  }
  return "unknown";
}

TEST_SUITE("integration") {

// ── Simple completion ────────────────────────────────────────────

TEST_CASE("simple completion across all providers and models") {
  for (auto p : agt::providers) {
    auto *key = std::getenv(provider_env_key(p));
    if (!key)
      continue;

    for (const auto &info : agt::curated_models(p)) {
      SUBCASE(info.id.c_str()) {
        std::cout << "  " << agt::provider_to_string(p) << "/" << info.id << " ... " << std::flush;

        agt::Llm llm(p, info.id, key);
        json req = {{"messages", json::array({{{"role", "user"}, {"content", "Reply with exactly: hello"}}})}};
        auto res = llm.complete(req);

        CHECK(res["stop_reason"] == "end");
        CHECK(res["content"].is_string());
        CHECK(res["usage"]["input_tokens"].get<int>() > 0);
        CHECK(res["usage"]["output_tokens"].get<int>() > 0);

        std::cout << "ok\n";
      }
    }
  }
}

// ── Streaming ────────────────────────────────────────────────────

TEST_CASE("streaming completion across all providers and models") {
  for (auto p : agt::providers) {
    auto *key = std::getenv(provider_env_key(p));
    if (!key)
      continue;

    for (const auto &info : agt::curated_models(p)) {
      SUBCASE(info.id.c_str()) {
        std::cout << "  " << agt::provider_to_string(p) << "/" << info.id << " stream ... " << std::flush;

        agt::Llm llm(p, info.id, key);
        json req = {{"messages", json::array({{{"role", "user"}, {"content", "Reply with exactly: hello"}}})}};

        std::string streamed;
        auto res = llm.complete(req, [&](const std::string &token) {
          streamed += token;
        });

        CHECK(res["stop_reason"] == "end");
        CHECK(res["content"].is_string());
        CHECK(res["usage"]["input_tokens"].get<int>() > 0);
        CHECK(res["usage"]["output_tokens"].get<int>() > 0);
        CHECK(streamed == res["content"].get<std::string>());

        std::cout << "ok\n";
      }
    }
  }
}

// ── Tool calling ─────────────────────────────────────────────────

TEST_CASE("tool calling round-trip across all providers and models") {
  json weather_tool = {
      {"name", "get_weather"},
      {"description", "Get current weather for a location"},
      {"parameters",
       {{"type", "object"},
        {"properties", {{"location", {{"type", "string"}, {"description", "City name"}}}}},
        {"required", json::array({"location"})}}}};

  for (auto p : agt::providers) {
    auto *key = std::getenv(provider_env_key(p));
    if (!key)
      continue;

    for (const auto &info : agt::curated_models(p)) {
      SUBCASE(info.id.c_str()) {
        std::cout << "  " << agt::provider_to_string(p) << "/" << info.id << " tool ... " << std::flush;

        agt::Llm llm(p, info.id, key);

        // Step 1: send user message with tool definition, expect tool_use
        json req = {{"messages", json::array({{{"role", "user"}, {"content", "What is the weather in Paris?"}}})},
                    {"tools", json::array({weather_tool})}};
        auto res = llm.complete(req);

        CHECK(res["stop_reason"] == "tool_use");
        REQUIRE(res.contains("calls"));
        REQUIRE(!res["calls"].empty());

        auto &call = res["calls"][0];
        CHECK(call.contains("name"));
        CHECK(call.contains("id"));
        CHECK(call.contains("input"));

        // Step 2: send tool result back, expect final response
        json assistant_msg = {{"role", "assistant"},
                              {"content", res["content"]},
                              {"calls", res["calls"]}};

        json messages = req["messages"];
        messages.push_back(std::move(assistant_msg));
        messages.push_back({{"role", "tool"},
                            {"call_id", call["id"]},
                            {"content", R"({"temperature":"18C","conditions":"cloudy"})"}});

        json req2 = {{"messages", messages}, {"tools", json::array({weather_tool})}};
        auto res2 = llm.complete(req2);

        CHECK(res2["stop_reason"] == "end");
        CHECK(res2["content"].is_string());

        std::cout << "ok\n";
      }
    }
  }
}

// ── Streaming tool calling ───────────────────────────────────────

TEST_CASE("streaming tool calling across all providers and models") {
  json weather_tool = {
      {"name", "get_weather"},
      {"description", "Get current weather for a location"},
      {"parameters",
       {{"type", "object"},
        {"properties", {{"location", {{"type", "string"}, {"description", "City name"}}}}},
        {"required", json::array({"location"})}}}};

  for (auto p : agt::providers) {
    auto *key = std::getenv(provider_env_key(p));
    if (!key)
      continue;

    for (const auto &info : agt::curated_models(p)) {
      SUBCASE(info.id.c_str()) {
        std::cout << "  " << agt::provider_to_string(p) << "/" << info.id << " stream-tool ... " << std::flush;

        agt::Llm llm(p, info.id, key);

        json req = {{"messages", json::array({{{"role", "user"}, {"content", "What is the weather in Paris?"}}})},
                    {"tools", json::array({weather_tool})}};

        std::string streamed;
        auto res = llm.complete(req, [&](const std::string &token) {
          streamed += token;
        });

        CHECK(res["stop_reason"] == "tool_use");
        REQUIRE(res.contains("calls"));
        REQUIRE(!res["calls"].empty());
        CHECK(res["calls"][0].contains("id"));
        CHECK(res["calls"][0].contains("name"));
        CHECK(res["calls"][0].contains("input"));

        std::cout << "ok\n";
      }
    }
  }
}

// ── Multi-turn with tool use ─────────────────────────────────────

TEST_CASE("multi-turn: follow-up after tool use does not reject null content") {
  json weather_tool = {
      {"name", "get_weather"},
      {"description", "Get current weather for a location"},
      {"parameters",
       {{"type", "object"},
        {"properties", {{"location", {{"type", "string"}, {"description", "City name"}}}}},
        {"required", json::array({"location"})}}}};

  for (auto p : agt::providers) {
    auto *key = std::getenv(provider_env_key(p));
    if (!key)
      continue;

    // Use first curated model only — this tests the session replay, not breadth
    auto models = agt::curated_models(p);
    REQUIRE(!models.empty());
    const auto &first = models[0];

    SUBCASE(first.id.c_str()) {
      std::cout << "  " << agt::provider_to_string(p) << "/" << first.id << " multi-turn ... " << std::flush;

      agt::Llm llm(p, first.id, key);

      // Turn 1: trigger tool use
      json messages = json::array({{{"role", "user"}, {"content", "What is the weather in Paris?"}}});
      json req = {{"messages", messages}, {"tools", json::array({weather_tool})}};
      auto res = llm.complete(req);

      REQUIRE(res["stop_reason"] == "tool_use");
      REQUIRE(res.contains("calls"));
      auto &call = res["calls"][0];

      // Append assistant (with null content) + tool result
      messages.push_back({{"role", "assistant"}, {"content", res["content"]}, {"calls", res["calls"]}});
      messages.push_back({{"role", "tool"}, {"call_id", call["id"]}, {"content", R"({"temperature":"18C"})"}});

      // Get the tool-use response
      json req2 = {{"messages", messages}, {"tools", json::array({weather_tool})}};
      auto res2 = llm.complete(req2);
      CHECK(res2["stop_reason"] == "end");

      // Append that response to history
      messages.push_back({{"role", "assistant"}, {"content", res2["content"]}});

      // Turn 2: follow-up with full history
      messages.push_back({{"role", "user"}, {"content", "What about London?"}});
      json req3 = {{"messages", messages}, {"tools", json::array({weather_tool})}};
      auto res3 = llm.complete(req3);

      CHECK((res3["stop_reason"] == "end" || res3["stop_reason"] == "tool_use"));

      std::cout << "ok\n";
    }
  }
}

// ── Thinking effort ──────────────────────────────────────────────

TEST_CASE("thinking effort accepted by models that support it") {
  for (auto p : agt::providers) {
    auto *key = std::getenv(provider_env_key(p));
    if (!key)
      continue;

    for (const auto &info : agt::curated_models(p)) {
      if (!info.supports_thinking)
        continue;

      SUBCASE(info.id.c_str()) {
        std::cout << "  " << agt::provider_to_string(p) << "/" << info.id << " thinking ... " << std::flush;

        agt::Llm llm(p, info.id, key);
        json req = {{"messages", json::array({{{"role", "user"}, {"content", "Reply with exactly: hello"}}})},
                    {"thinking_effort", "low"}};
        auto res = llm.complete(req);

        CHECK(res["stop_reason"] == "end");
        CHECK(res["content"].is_string());

        std::cout << "ok\n";
      }
    }
  }
}

TEST_CASE("thinking effort silently ignored by models that do not support it") {
  for (auto p : agt::providers) {
    auto *key = std::getenv(provider_env_key(p));
    if (!key)
      continue;

    for (const auto &info : agt::curated_models(p)) {
      if (info.supports_thinking)
        continue;

      SUBCASE(info.id.c_str()) {
        std::cout << "  " << agt::provider_to_string(p) << "/" << info.id << " no-thinking ... " << std::flush;

        agt::Llm llm(p, info.id, key);
        json req = {{"messages", json::array({{{"role", "user"}, {"content", "Reply with exactly: hello"}}})},
                    {"thinking_effort", "high"}};
        auto res = llm.complete(req);

        // Should succeed without error — thinking_effort is silently dropped
        CHECK(res["stop_reason"] == "end");
        CHECK(res["content"].is_string());

        std::cout << "ok\n";
      }
    }
  }
}

// ── Response schema validation ───────────────────────────────────

TEST_CASE("all responses conform to canonical output schema") {
  json weather_tool = {
      {"name", "get_weather"},
      {"description", "Get current weather for a location"},
      {"parameters",
       {{"type", "object"},
        {"properties", {{"location", {{"type", "string"}, {"description", "City name"}}}}},
        {"required", json::array({"location"})}}}};

  for (auto p : agt::providers) {
    auto *key = std::getenv(provider_env_key(p));
    if (!key)
      continue;

    for (const auto &info : agt::curated_models(p)) {
      SUBCASE(info.id.c_str()) {
        std::cout << "  " << agt::provider_to_string(p) << "/" << info.id << " schema ... " << std::flush;

        agt::Llm llm(p, info.id, key);

        // Plain completion
        json req1 = {{"messages", json::array({{{"role", "user"}, {"content", "Say hi"}}})}};
        auto res1 = llm.complete(req1);
        CHECK(res1.contains("stop_reason"));
        CHECK(res1.contains("usage"));
        CHECK(res1["usage"].contains("input_tokens"));
        CHECK(res1["usage"].contains("output_tokens"));

        // Tool call
        json req2 = {{"messages", json::array({{{"role", "user"}, {"content", "What is the weather in Paris?"}}})},
                     {"tools", json::array({weather_tool})}};
        auto res2 = llm.complete(req2);
        CHECK(res2.contains("stop_reason"));
        CHECK(res2.contains("usage"));
        if (res2["stop_reason"] == "tool_use") {
          REQUIRE(res2.contains("calls"));
          for (const auto &call : res2["calls"]) {
            CHECK(call.contains("id"));
            CHECK(call.contains("name"));
            CHECK(call.contains("input"));
            // input must be a valid JSON string
            auto parsed = json::parse(call["input"].get<std::string>(), nullptr, false);
            CHECK_FALSE(parsed.is_discarded());
          }
        }

        std::cout << "ok\n";
      }
    }
  }
}

} // TEST_SUITE
