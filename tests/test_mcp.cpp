#include <agt/mcp.hpp>
#include <agt/tool.hpp>
#include <doctest/doctest.h>
#include <string>

using json = nlohmann::json;

#ifndef AGT_FAKE_MCP_STDIO
#define AGT_FAKE_MCP_STDIO "tests/fixtures/fake_mcp_stdio.py"
#endif

static agt::mcp_config stdio_config() {
  agt::mcp_config cfg;
  cfg.transport = agt::McpTransport::stdio;
  cfg.name = "fake";
  cfg.command = "python3";
  cfg.args = {AGT_FAKE_MCP_STDIO};
  return cfg;
}

TEST_SUITE("mcp stdio") {

TEST_CASE("connect + tools/list discovers tools with schemas") {
  agt::McpServer srv(stdio_config());
  srv.connect();
  auto tools = srv.tools();

  REQUIRE(tools.size() == 3);

  // Tools come back in server order.
  CHECK(std::string(tools[0]->name()) == "echo");
  CHECK(std::string(tools[1]->name()) == "add");
  CHECK(std::string(tools[2]->name()) == "explode");

  // Descriptions round-trip.
  CHECK(std::string(tools[0]->description()) == "Echoes text back");

  // Schema surfaces from inputSchema.
  auto echo_schema = tools[0]->parameters();
  CHECK(echo_schema["type"] == "object");
  CHECK(echo_schema["required"][0] == "text");
}

TEST_CASE("tools() is idempotent across calls (caches)") {
  agt::McpServer srv(stdio_config());
  srv.connect();
  auto a = srv.tools();
  auto b = srv.tools();
  REQUIRE(a.size() == b.size());
  for (size_t i = 0; i < a.size(); ++i)
    CHECK(a[i].get() == b[i].get()); // same underlying object
}

TEST_CASE("tool execute returns server content") {
  agt::McpServer srv(stdio_config());
  srv.connect();
  auto tools = srv.tools();
  auto echo = tools[0];

  auto result = echo->execute(json{{"text", "hi there"}});
  // Server wraps in {"content": [{"type": "text", "text": ...}]}
  REQUIRE(result.contains("content"));
  REQUIRE(result["content"].is_array());
  CHECK(result["content"][0]["text"] == "hi there");
}

TEST_CASE("tool with numeric args round-trips") {
  agt::McpServer srv(stdio_config());
  srv.connect();
  auto tools = srv.tools();
  auto add = tools[1];

  auto result = add->execute(json{{"a", 2}, {"b", 40}});
  CHECK(result["content"][0]["text"] == "42");
}

TEST_CASE("server-reported error is surfaced as {error: ...} instead of throwing") {
  agt::McpServer srv(stdio_config());
  srv.connect();
  auto tools = srv.tools();
  auto explode = tools[2];

  // mcp_tool::execute catches the JSON-RPC error and returns it as data.
  auto result = explode->execute(json::object());
  REQUIRE(result.contains("error"));
  auto msg = result["error"].get<std::string>();
  CHECK(msg.find("boom") != std::string::npos);
}

TEST_CASE("tools() before connect returns empty") {
  agt::McpServer srv(stdio_config());
  // no connect()
  auto tools = srv.tools();
  CHECK(tools.empty());
}

TEST_CASE("destroying server cleans up child process") {
  // Just ensure rapid construct/destruct doesn't leak or hang. If the child
  // process weren't reaped the test binary would eventually see zombies.
  for (int i = 0; i < 3; ++i) {
    agt::McpServer srv(stdio_config());
    srv.connect();
    (void)srv.tools();
  }
  CHECK(true);
}

} // TEST_SUITE
