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

TEST_CASE("simple completion across curated models") {
  for (auto p : agt::providers) {
    auto *key = std::getenv(provider_env_key(p));
    if (!key)
      continue;

    auto models = agt::curated_models(p);
    for (auto &model : models) {
      SUBCASE(model.c_str()) {
        std::cout << "  " << agt::provider_to_string(p) << "/" << model << " ... " << std::flush;

        agt::Llm llm(p, model, key);
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

TEST_CASE("tool calling round-trip across curated models") {
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

    auto models = agt::curated_models(p);
    for (auto &model : models) {
      SUBCASE(model.c_str()) {
        std::cout << "  " << agt::provider_to_string(p) << "/" << model << " tool ... " << std::flush;

        agt::Llm llm(p, model, key);

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

        // Step 2: send tool result back, expect final response
        // Use the content as-is from the response (null or string).
        // Anthropic rejects empty string content, so preserve null when null.
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

        std::cout << "ok\n";
      }
    }
  }
}

TEST_CASE("follow-up after tool use does not reject null content") {
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
    auto &model = models[0];

    SUBCASE(model.c_str()) {
      std::cout << "  " << agt::provider_to_string(p) << "/" << model << " multi-turn ... " << std::flush;

      agt::Llm llm(p, model, key);

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

      // Turn 2: follow-up message with full history (the bug: null content in
      // the earlier assistant message causes "text content blocks must be non-empty")
      messages.push_back({{"role", "user"}, {"content", "What about London?"}});
      json req3 = {{"messages", messages}, {"tools", json::array({weather_tool})}};
      auto res3 = llm.complete(req3);

      CHECK((res3["stop_reason"] == "end" || res3["stop_reason"] == "tool_use"));

      std::cout << "ok\n";
    }
  }
}

TEST_CASE("streaming completion across curated models") {
  for (auto p : agt::providers) {
    auto *key = std::getenv(provider_env_key(p));
    if (!key)
      continue;

    auto models = agt::curated_models(p);
    for (auto &model : models) {
      SUBCASE(model.c_str()) {
        std::cout << "  " << agt::provider_to_string(p) << "/" << model << " stream ... " << std::flush;

        agt::Llm llm(p, model, key);
        json req = {{"messages", json::array({{{"role", "user"}, {"content", "Reply with exactly: hello"}}})}};

        std::string streamed;
        auto res = llm.complete(req, [&](const std::string &token) {
          streamed += token;
        });

        CHECK(res["stop_reason"] == "end");
        CHECK(res["content"].is_string());
        CHECK(res["usage"]["input_tokens"].get<int>() > 0);
        CHECK(res["usage"]["output_tokens"].get<int>() > 0);

        // Streamed tokens should match final content.
        CHECK(streamed == res["content"].get<std::string>());

        std::cout << "ok\n";
      }
    }
  }
}

} // TEST_SUITE
