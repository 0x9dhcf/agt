// Validates that agt::Llm round-trips through an OpenAI-compatible fake
// server when AGT_OPENAI_BASE_URL redirects the hardcoded endpoint. Lets CI
// and the mission-control contract suite exercise the LLM wrapper without
// real API keys.

#include <agt/llm.hpp>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <doctest/doctest.h>
#include <fcntl.h>
#include <signal.h>
#include <string>
#include <sys/wait.h>
#include <unistd.h>

using json = nlohmann::json;

#ifndef AGT_FAKE_OPENAI_LLM
#define AGT_FAKE_OPENAI_LLM "tests/fixtures/fake_openai_llm.py"
#endif

namespace {

// Same helper shape as the mcp test — spawn python, read port from stdout,
// kill on destruction. Duplicated rather than shared to keep translation-unit
// boundaries clean; total is ~30 LOC per copy.
struct PyFixtureProcess {
  pid_t pid = -1;
  int port = 0;

  PyFixtureProcess(const char *script) {
    int pipefd[2];
    REQUIRE(pipe(pipefd) == 0);
    pid = fork();
    REQUIRE(pid >= 0);
    if (pid == 0) {
      ::close(pipefd[0]);
      dup2(pipefd[1], STDOUT_FILENO);
      ::close(pipefd[1]);
      execlp("python3", "python3", script, nullptr);
      _exit(127);
    }
    ::close(pipefd[1]);
    char buf[64] = {};
    ssize_t got = 0;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (std::chrono::steady_clock::now() < deadline && got < (ssize_t)sizeof(buf) - 1) {
      ssize_t n = ::read(pipefd[0], buf + got, sizeof(buf) - 1 - got);
      if (n <= 0) break;
      got += n;
      if (std::memchr(buf, '\n', got)) break;
    }
    ::close(pipefd[0]);
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

} // namespace

TEST_SUITE("llm fake (openai)") {

TEST_CASE("agt::Llm::complete round-trips against a fake OpenAI server") {
  PyFixtureProcess fake(AGT_FAKE_OPENAI_LLM);
  const std::string base_url =
      "http://127.0.0.1:" + std::to_string(fake.port) + "/v1/chat/completions";
  // setenv survives for the lifetime of this test case; we restore at end.
  setenv("AGT_OPENAI_BASE_URL", base_url.c_str(), 1);

  try {
    agt::Llm llm(agt::Provider::openai, "fake-model", "ignored-key");
    agt::Json input;
    input["messages"] = agt::Json::array({
        {{"role", "user"}, {"content", "what's up"}},
    });
    agt::Json out = llm.complete(input);
    // Canonical response shape is {content, calls?, stop_reason, usage?}.
    REQUIRE(out.contains("content"));
    const std::string content = out.value("content", std::string{});
    CHECK(content.find("Hello from fake LLM") != std::string::npos);
    // The fixture also echoes back the last user message — verifying our
    // canonical input actually made it through the wrapper.
    CHECK(content.find("what's up") != std::string::npos);
    CHECK(out.value("stop_reason", std::string{}) == "end");
  } catch (...) {
    unsetenv("AGT_OPENAI_BASE_URL");
    throw;
  }
  unsetenv("AGT_OPENAI_BASE_URL");
}

} // TEST_SUITE
