#include <doctest/doctest.h>
#include <agt/session.hpp>
#include <cstdio>
#include <string>

using json = nlohmann::json;

// Helper: unique temp DB path per test to avoid interference.
static std::string tmp_db() {
  static int counter = 0;
  return "/tmp/agt_test_sqlite_" + std::to_string(getpid()) + "_" +
         std::to_string(counter++) + ".db";
}

struct db_cleanup {
  std::string path;
  ~db_cleanup() { std::remove(path.c_str()); }
};

TEST_SUITE("sqlite_session") {

TEST_CASE("empty session returns empty array") {
  auto path = tmp_db();
  db_cleanup cleanup{path};
  auto s = agt::make_sqlite_session(path, "s1");
  auto msgs = s->messages();
  CHECK(msgs.is_array());
  CHECK(msgs.empty());
}

TEST_CASE("append adds messages and preserves order") {
  auto path = tmp_db();
  db_cleanup cleanup{path};
  auto s = agt::make_sqlite_session(path, "s1");
  s->append(json::array({{{"role", "user"}, {"content", "hello"}}}));
  s->append(json::array({{{"role", "assistant"}, {"content", "hi"}}}));

  auto msgs = s->messages();
  REQUIRE(msgs.size() == 2);
  CHECK(msgs[0]["role"] == "user");
  CHECK(msgs[0]["content"] == "hello");
  CHECK(msgs[1]["role"] == "assistant");
  CHECK(msgs[1]["content"] == "hi");
}

TEST_CASE("messages(N) returns last N") {
  auto path = tmp_db();
  db_cleanup cleanup{path};
  auto s = agt::make_sqlite_session(path, "s1");
  s->append(json::array({
      {{"role", "user"}, {"content", "a"}},
      {{"role", "assistant"}, {"content", "b"}},
      {{"role", "user"}, {"content", "c"}},
  }));

  auto last2 = s->messages(2);
  REQUIRE(last2.size() == 2);
  CHECK(last2[0]["content"] == "b");
  CHECK(last2[1]["content"] == "c");
}

TEST_CASE("messages(0) returns all") {
  auto path = tmp_db();
  db_cleanup cleanup{path};
  auto s = agt::make_sqlite_session(path, "s1");
  s->append(json::array({{{"role", "user"}, {"content", "a"}}}));
  CHECK(s->messages(0).size() == 1);
}

TEST_CASE("messages(N) where N > size returns all") {
  auto path = tmp_db();
  db_cleanup cleanup{path};
  auto s = agt::make_sqlite_session(path, "s1");
  s->append(json::array({{{"role", "user"}, {"content", "a"}}}));
  CHECK(s->messages(100).size() == 1);
}

TEST_CASE("replace atomically swaps history") {
  auto path = tmp_db();
  db_cleanup cleanup{path};
  auto s = agt::make_sqlite_session(path, "s1");
  s->append(json::array({{{"role", "user"}, {"content", "old"}}}));

  s->replace(json::array({{{"role", "user"}, {"content", "new"}}}));

  auto msgs = s->messages();
  REQUIRE(msgs.size() == 1);
  CHECK(msgs[0]["content"] == "new");
}

TEST_CASE("clear empties, subsequent append works") {
  auto path = tmp_db();
  db_cleanup cleanup{path};
  auto s = agt::make_sqlite_session(path, "s1");
  s->append(json::array({{{"role", "user"}, {"content", "a"}}}));
  s->clear();
  CHECK(s->messages().empty());

  s->append(json::array({{{"role", "user"}, {"content", "b"}}}));
  REQUIRE(s->messages().size() == 1);
  CHECK(s->messages()[0]["content"] == "b");
}

TEST_CASE("compact with keep >= size is a no-op") {
  auto path = tmp_db();
  db_cleanup cleanup{path};
  auto s = agt::make_sqlite_session(path, "s1");
  s->append(json::array({
      {{"role", "user"}, {"content", "a"}},
      {{"role", "assistant"}, {"content", "b"}},
  }));
  s->compact(5);
  CHECK(s->messages().size() == 2);
  s->compact(2);
  CHECK(s->messages().size() == 2);
}

TEST_CASE("compact keeps last N messages") {
  auto path = tmp_db();
  db_cleanup cleanup{path};
  auto s = agt::make_sqlite_session(path, "s1");
  s->append(json::array({
      {{"role", "user"}, {"content", "a"}},
      {{"role", "assistant"}, {"content", "b"}},
      {{"role", "user"}, {"content", "c"}},
      {{"role", "assistant"}, {"content", "d"}},
  }));
  s->compact(2);
  auto msgs = s->messages();
  REQUIRE(msgs.size() == 2);
  CHECK(msgs[0]["content"] == "c");
  CHECK(msgs[1]["content"] == "d");
}

TEST_CASE("compact skips past orphaned tool results at cut boundary") {
  auto path = tmp_db();
  db_cleanup cleanup{path};
  auto s = agt::make_sqlite_session(path, "s1");
  s->append(json::array({
      {{"role", "user"}, {"content", "q1"}},
      {{"role", "assistant"}, {"content", "a1"}, {"calls", json::array({{{"id", "c1"}, {"name", "t"}, {"input", "{}"}}})}},
      {{"role", "tool"}, {"call_id", "c1"}, {"content", "r1"}},
      {{"role", "user"}, {"content", "q2"}},
      {{"role", "assistant"}, {"content", "a2"}},
  }));
  // keep=3 would cut at index 2, which is a tool result — should skip forward
  s->compact(3);
  auto msgs = s->messages();
  REQUIRE(msgs.size() == 2);
  CHECK(msgs[0]["content"] == "q2");
  CHECK(msgs[1]["content"] == "a2");
}

TEST_CASE("compact on empty session is a no-op") {
  auto path = tmp_db();
  db_cleanup cleanup{path};
  auto s = agt::make_sqlite_session(path, "s1");
  s->compact(5);
  CHECK(s->messages().empty());
}

TEST_CASE("compact with keep <= 0 is a no-op") {
  auto path = tmp_db();
  db_cleanup cleanup{path};
  auto s = agt::make_sqlite_session(path, "s1");
  s->append(json::array({{{"role", "user"}, {"content", "a"}}}));
  s->compact(0);
  CHECK(s->messages().size() == 1);
  s->compact(-1);
  CHECK(s->messages().size() == 1);
}

TEST_CASE("persistence: messages survive object destruction") {
  auto path = tmp_db();
  db_cleanup cleanup{path};

  {
    auto s = agt::make_sqlite_session(path, "s1");
    s->append(json::array({
        {{"role", "user"}, {"content", "hello"}},
        {{"role", "assistant"}, {"content", "hi"}},
    }));
  }

  auto s = agt::make_sqlite_session(path, "s1");
  auto msgs = s->messages();
  REQUIRE(msgs.size() == 2);
  CHECK(msgs[0]["content"] == "hello");
  CHECK(msgs[1]["content"] == "hi");
}

TEST_CASE("multiple session IDs do not interfere") {
  auto path = tmp_db();
  db_cleanup cleanup{path};

  auto s1 = agt::make_sqlite_session(path, "session-a");
  auto s2 = agt::make_sqlite_session(path, "session-b");

  s1->append(json::array({{{"role", "user"}, {"content", "from-a"}}}));
  s2->append(json::array({{{"role", "user"}, {"content", "from-b"}}}));

  auto msgs1 = s1->messages();
  auto msgs2 = s2->messages();
  REQUIRE(msgs1.size() == 1);
  REQUIRE(msgs2.size() == 1);
  CHECK(msgs1[0]["content"] == "from-a");
  CHECK(msgs2[0]["content"] == "from-b");

  s1->clear();
  CHECK(s1->messages().empty());
  CHECK(s2->messages().size() == 1);
}

TEST_CASE("unicode and nested JSON content round-trip") {
  auto path = tmp_db();
  db_cleanup cleanup{path};
  auto s = agt::make_sqlite_session(path, "s1");

  json nested = {{"list", json::array({1, 2, 3})},
                 {"obj", {{"k", "日本語"}, {"emoji", "🌙✨"}}}};
  s->append(json::array({
      {{"role", "user"}, {"content", "naïve → café — Ω 🚀"}},
      {{"role", "assistant"}, {"content", nested.dump()}},
  }));

  auto msgs = s->messages();
  REQUIRE(msgs.size() == 2);
  CHECK(msgs[0]["content"] == "naïve → café — Ω 🚀");
  CHECK(json::parse(msgs[1]["content"].get<std::string>()) == nested);
}

TEST_CASE("large payload (~200KB) round-trips intact") {
  auto path = tmp_db();
  db_cleanup cleanup{path};
  auto s = agt::make_sqlite_session(path, "s1");

  std::string big(200 * 1024, 'x');
  big[100] = 'Z';
  big.back() = 'Q';
  s->append(json::array({{{"role", "user"}, {"content", big}}}));

  auto msgs = s->messages();
  REQUIRE(msgs.size() == 1);
  auto got = msgs[0]["content"].get<std::string>();
  CHECK(got.size() == big.size());
  CHECK(got[100] == 'Z');
  CHECK(got.back() == 'Q');
}

TEST_CASE("two handles on same (path, session_id) see each other's writes") {
  auto path = tmp_db();
  db_cleanup cleanup{path};

  auto s1 = agt::make_sqlite_session(path, "shared");
  auto s2 = agt::make_sqlite_session(path, "shared");

  s1->append(json::array({{{"role", "user"}, {"content", "from-1"}}}));
  auto seen = s2->messages();
  REQUIRE(seen.size() == 1);
  CHECK(seen[0]["content"] == "from-1");

  s2->append(json::array({{{"role", "assistant"}, {"content", "from-2"}}}));
  auto seen_back = s1->messages();
  REQUIRE(seen_back.size() == 2);
  CHECK(seen_back[1]["content"] == "from-2");
}

TEST_CASE("replace then reopen preserves the replacement") {
  auto path = tmp_db();
  db_cleanup cleanup{path};

  {
    auto s = agt::make_sqlite_session(path, "s1");
    s->append(json::array({{{"role", "user"}, {"content", "old-a"}},
                            {{"role", "user"}, {"content", "old-b"}}}));
    s->replace(json::array({{{"role", "user"}, {"content", "fresh"}}}));
  }

  auto s = agt::make_sqlite_session(path, "s1");
  auto msgs = s->messages();
  REQUIRE(msgs.size() == 1);
  CHECK(msgs[0]["content"] == "fresh");
}

TEST_CASE("compact persists across reopen") {
  auto path = tmp_db();
  db_cleanup cleanup{path};

  {
    auto s = agt::make_sqlite_session(path, "s1");
    s->append(json::array({
        {{"role", "user"}, {"content", "a"}},
        {{"role", "assistant"}, {"content", "b"}},
        {{"role", "user"}, {"content", "c"}},
        {{"role", "assistant"}, {"content", "d"}},
    }));
    s->compact(2);
  }

  auto s = agt::make_sqlite_session(path, "s1");
  auto msgs = s->messages();
  REQUIRE(msgs.size() == 2);
  CHECK(msgs[0]["content"] == "c");
  CHECK(msgs[1]["content"] == "d");
}

TEST_CASE("tool calls with call_id and calls round-trip correctly") {
  auto path = tmp_db();
  db_cleanup cleanup{path};
  auto s = agt::make_sqlite_session(path, "s1");

  json calls = json::array({{{"id", "call_1"}, {"name", "my_tool"}, {"input", "{\"x\":1}"}}});
  s->append(json::array({
      {{"role", "assistant"}, {"content", "thinking"}, {"calls", calls}},
      {{"role", "tool"}, {"call_id", "call_1"}, {"content", "result"}},
  }));

  auto msgs = s->messages();
  REQUIRE(msgs.size() == 2);
  CHECK(msgs[0]["calls"] == calls);
  CHECK(msgs[1]["call_id"] == "call_1");
}

} // TEST_SUITE
