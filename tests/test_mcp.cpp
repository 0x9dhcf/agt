#include <agt/mcp.hpp>
#include <agt/tool.hpp>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <doctest/doctest.h>
#include <fcntl.h>
#include <signal.h>
#include <string>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>

using json = nlohmann::json;

#ifndef AGT_FAKE_MCP_STDIO
#define AGT_FAKE_MCP_STDIO "tests/fixtures/fake_mcp_stdio.py"
#endif
#ifndef AGT_FAKE_MCP_HTTP
#define AGT_FAKE_MCP_HTTP "tests/fixtures/fake_mcp_http.py"
#endif
#ifndef AGT_FAKE_MCP_SSE
#define AGT_FAKE_MCP_SSE "tests/fixtures/fake_mcp_sse.py"
#endif

static agt::mcp_config stdio_config() {
  agt::mcp_config cfg;
  cfg.transport = agt::McpTransport::stdio;
  cfg.name = "fake";
  cfg.command = "python3";
  cfg.args = {AGT_FAKE_MCP_STDIO};
  return cfg;
}

// RAII helper for a Python fixture server that prints its listening port on
// stdout. Spawns the script, reads the first line for the port, and SIGTERMs
// the process on destruction. Stays simple — no pipe-based stderr capture,
// no timeout knobs; tests don't need them.
struct PyFixtureProcess {
  pid_t pid = -1;
  int port = 0;

  PyFixtureProcess(const char *script) {
    int pipefd[2];
    REQUIRE(pipe(pipefd) == 0);
    pid = fork();
    REQUIRE(pid >= 0);
    if (pid == 0) {
      // Child
      ::close(pipefd[0]);
      dup2(pipefd[1], STDOUT_FILENO);
      ::close(pipefd[1]);
      execlp("python3", "python3", script, nullptr);
      _exit(127);
    }
    ::close(pipefd[1]);
    // Read one line (the port) from the pipe. Spin until we get it.
    char buf[64] = {};
    ssize_t n = 0;
    ssize_t got = 0;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (std::chrono::steady_clock::now() < deadline && got < (ssize_t)sizeof(buf) - 1) {
      n = ::read(pipefd[0], buf + got, sizeof(buf) - 1 - got);
      if (n <= 0) break;
      got += n;
      if (std::memchr(buf, '\n', got)) break;
    }
    ::close(pipefd[0]);
    buf[sizeof(buf) - 1] = 0;
    port = std::atoi(buf);
    REQUIRE(port > 0);
  }
  ~PyFixtureProcess() {
    if (pid > 0) {
      kill(pid, SIGTERM);
      waitpid(pid, nullptr, 0);
    }
  }
  PyFixtureProcess(const PyFixtureProcess &) = delete;
  PyFixtureProcess &operator=(const PyFixtureProcess &) = delete;
};

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

// ---------------------------------------------------------------------------
// HTTP transport — parity with stdio, backed by fake_mcp_http.py
// ---------------------------------------------------------------------------

TEST_SUITE("mcp http") {

TEST_CASE("http transport: connect + tools/list + call") {
  PyFixtureProcess fixture(AGT_FAKE_MCP_HTTP);
  agt::mcp_config cfg;
  cfg.transport = agt::McpTransport::http;
  cfg.name = "fake-http";
  cfg.command = "http://127.0.0.1:" + std::to_string(fixture.port) + "/";

  agt::McpServer srv(cfg);
  srv.connect();
  auto tools = srv.tools();
  REQUIRE(tools.size() == 3);
  CHECK(std::string(tools[0]->name()) == "echo");

  auto result = tools[0]->execute(json{{"text", "hi http"}});
  CHECK(result["content"][0]["text"] == "hi http");
}

TEST_CASE("http transport: server-reported error surfaces as {error}") {
  PyFixtureProcess fixture(AGT_FAKE_MCP_HTTP);
  agt::mcp_config cfg;
  cfg.transport = agt::McpTransport::http;
  cfg.name = "fake-http";
  cfg.command = "http://127.0.0.1:" + std::to_string(fixture.port) + "/";

  agt::McpServer srv(cfg);
  srv.connect();
  auto tools = srv.tools();
  auto result = tools[2]->execute(json::object());
  REQUIRE(result.contains("error"));
  CHECK(result["error"].get<std::string>().find("boom") != std::string::npos);
}

} // TEST_SUITE

// ---------------------------------------------------------------------------
// SSE transport — two-channel: GET /sse for events, POST /messages?sid=...
// for JSON-RPC requests. URL auto-upgrade (http + path ending /sse → sse) is
// covered separately.
// ---------------------------------------------------------------------------

TEST_SUITE("mcp sse") {

TEST_CASE("sse transport: connect + tools/list + call") {
  PyFixtureProcess fixture(AGT_FAKE_MCP_SSE);
  agt::mcp_config cfg;
  cfg.transport = agt::McpTransport::sse;
  cfg.name = "fake-sse";
  cfg.command = "http://127.0.0.1:" + std::to_string(fixture.port) + "/sse";

  agt::McpServer srv(cfg);
  srv.connect();
  auto tools = srv.tools();
  REQUIRE(tools.size() == 3);
  CHECK(std::string(tools[0]->name()) == "echo");

  auto result = tools[0]->execute(json{{"text", "hi sse"}});
  CHECK(result["content"][0]["text"] == "hi sse");
}

TEST_CASE("sse transport: add tool numeric roundtrip") {
  PyFixtureProcess fixture(AGT_FAKE_MCP_SSE);
  agt::mcp_config cfg;
  cfg.transport = agt::McpTransport::sse;
  cfg.name = "fake-sse";
  cfg.command = "http://127.0.0.1:" + std::to_string(fixture.port) + "/sse";

  agt::McpServer srv(cfg);
  srv.connect();
  auto tools = srv.tools();
  auto result = tools[1]->execute(json{{"a", 2}, {"b", 40}});
  CHECK(result["content"][0]["text"] == "42");
}

TEST_CASE("sse transport: server error surfaces through pending queue") {
  PyFixtureProcess fixture(AGT_FAKE_MCP_SSE);
  agt::mcp_config cfg;
  cfg.transport = agt::McpTransport::sse;
  cfg.name = "fake-sse";
  cfg.command = "http://127.0.0.1:" + std::to_string(fixture.port) + "/sse";

  agt::McpServer srv(cfg);
  srv.connect();
  auto tools = srv.tools();
  auto result = tools[2]->execute(json::object());
  REQUIRE(result.contains("error"));
  CHECK(result["error"].get<std::string>().find("boom") != std::string::npos);
}

TEST_CASE("sse transport: url-looks-like-sse auto-upgrades http config") {
  // Caller says transport=http but URL ends in /sse — McpServer flips to sse
  // transparently at connect time. Matches the original draft's behaviour and
  // keeps older callers (like the UI's hard-coded transport: 'http') working.
  PyFixtureProcess fixture(AGT_FAKE_MCP_SSE);
  agt::mcp_config cfg;
  cfg.transport = agt::McpTransport::http;  // misconfigured on purpose
  cfg.name = "fake-sse";
  cfg.command = "http://127.0.0.1:" + std::to_string(fixture.port) + "/sse";

  agt::McpServer srv(cfg);
  srv.connect();  // would 405 without auto-upgrade
  auto tools = srv.tools();
  REQUIRE(tools.size() == 3);
  auto result = tools[0]->execute(json{{"text", "auto-upgrade"}});
  CHECK(result["content"][0]["text"] == "auto-upgrade");
}

} // TEST_SUITE
