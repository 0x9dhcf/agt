#include <doctest/doctest.h>
#include <agt/session.hpp>

using json = nlohmann::json;

TEST_SUITE("memory_session") {

TEST_CASE("empty session returns empty array") {
  agt::MemorySession s;
  auto msgs = s.messages();
  CHECK(msgs.is_array());
  CHECK(msgs.empty());
}

TEST_CASE("append adds messages and preserves order") {
  agt::MemorySession s;
  s.append(json::array({{{"role", "user"}, {"content", "hello"}}}));
  s.append(json::array({{{"role", "assistant"}, {"content", "hi"}}}));

  auto msgs = s.messages();
  REQUIRE(msgs.size() == 2);
  CHECK(msgs[0]["role"] == "user");
  CHECK(msgs[0]["content"] == "hello");
  CHECK(msgs[1]["role"] == "assistant");
  CHECK(msgs[1]["content"] == "hi");
}

TEST_CASE("messages(N) returns last N") {
  agt::MemorySession s;
  s.append(json::array({
      {{"role", "user"}, {"content", "a"}},
      {{"role", "assistant"}, {"content", "b"}},
      {{"role", "user"}, {"content", "c"}},
  }));

  auto last2 = s.messages(2);
  REQUIRE(last2.size() == 2);
  CHECK(last2[0]["content"] == "b");
  CHECK(last2[1]["content"] == "c");
}

TEST_CASE("messages(0) returns all") {
  agt::MemorySession s;
  s.append(json::array({{{"role", "user"}, {"content", "a"}}}));
  CHECK(s.messages(0).size() == 1);
}

TEST_CASE("messages(-1) returns all") {
  agt::MemorySession s;
  s.append(json::array({{{"role", "user"}, {"content", "a"}}}));
  CHECK(s.messages(-1).size() == 1);
}

TEST_CASE("messages(N) where N > size returns all") {
  agt::MemorySession s;
  s.append(json::array({{{"role", "user"}, {"content", "a"}}}));
  CHECK(s.messages(100).size() == 1);
}

TEST_CASE("replace atomically swaps history") {
  agt::MemorySession s;
  s.append(json::array({{{"role", "user"}, {"content", "old"}}}));

  s.replace(json::array({{{"role", "user"}, {"content", "new"}}}));

  auto msgs = s.messages();
  REQUIRE(msgs.size() == 1);
  CHECK(msgs[0]["content"] == "new");
}

TEST_CASE("clear empties, subsequent append works") {
  agt::MemorySession s;
  s.append(json::array({{{"role", "user"}, {"content", "a"}}}));
  s.clear();
  CHECK(s.messages().empty());

  s.append(json::array({{{"role", "user"}, {"content", "b"}}}));
  REQUIRE(s.messages().size() == 1);
  CHECK(s.messages()[0]["content"] == "b");
}

TEST_CASE("compact with keep >= size is a no-op") {
  agt::MemorySession s;
  s.append(json::array({
      {{"role", "user"}, {"content", "a"}},
      {{"role", "assistant"}, {"content", "b"}},
  }));
  s.compact(5);
  CHECK(s.messages().size() == 2);
  s.compact(2);
  CHECK(s.messages().size() == 2);
}

TEST_CASE("compact keeps last N messages") {
  agt::MemorySession s;
  s.append(json::array({
      {{"role", "user"}, {"content", "a"}},
      {{"role", "assistant"}, {"content", "b"}},
      {{"role", "user"}, {"content", "c"}},
      {{"role", "assistant"}, {"content", "d"}},
  }));
  s.compact(2);
  auto msgs = s.messages();
  REQUIRE(msgs.size() == 2);
  CHECK(msgs[0]["content"] == "c");
  CHECK(msgs[1]["content"] == "d");
}

TEST_CASE("compact skips past orphaned tool results at cut boundary") {
  agt::MemorySession s;
  s.append(json::array({
      {{"role", "user"}, {"content", "q1"}},
      {{"role", "assistant"}, {"content", "a1"}, {"calls", json::array({{{"id", "c1"}, {"name", "t"}, {"input", "{}"}}})}},
      {{"role", "tool"}, {"call_id", "c1"}, {"content", "r1"}},
      {{"role", "user"}, {"content", "q2"}},
      {{"role", "assistant"}, {"content", "a2"}},
  }));
  // keep=3 would cut at index 2, which is a tool result — should skip forward
  s.compact(3);
  auto msgs = s.messages();
  REQUIRE(msgs.size() == 2);
  CHECK(msgs[0]["content"] == "q2");
  CHECK(msgs[1]["content"] == "a2");
}

TEST_CASE("compact on empty session is a no-op") {
  agt::MemorySession s;
  s.compact(5);
  CHECK(s.messages().empty());
}

TEST_CASE("compact with keep <= 0 is a no-op") {
  agt::MemorySession s;
  s.append(json::array({{{"role", "user"}, {"content", "a"}}}));
  s.compact(0);
  CHECK(s.messages().size() == 1);
  s.compact(-1);
  CHECK(s.messages().size() == 1);
}

} // TEST_SUITE
