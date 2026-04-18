#include <agt/session.hpp>
#include <cstdlib>
#include <doctest/doctest.h>
#include <string>
#include <unistd.h>

using json = nlohmann::json;

namespace {

const char* pg_dsn() {
  const char* v = std::getenv("PG_TEST_DSN");
  return (v != nullptr && *v != '\0') ? v : nullptr;
}

// Each test uses a distinct session_id so we never step on each other's data,
// even when running back-to-back against the same Postgres.
std::string fresh_session() {
  static int counter = 0;
  return "pg-test-" + std::to_string(getpid()) + "-" + std::to_string(counter++);
}

} // namespace

TEST_SUITE("pg_session" * doctest::skip(pg_dsn() == nullptr)) {

TEST_CASE("empty session returns empty array") {
  auto s = agt::make_pg_session(pg_dsn(), fresh_session());
  auto msgs = s->messages();
  CHECK(msgs.is_array());
  CHECK(msgs.empty());
}

TEST_CASE("append + messages preserves order") {
  auto s = agt::make_pg_session(pg_dsn(), fresh_session());
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
  auto s = agt::make_pg_session(pg_dsn(), fresh_session());
  s->append(json::array({
      {{"role", "user"}, {"content", "m1"}},
      {{"role", "assistant"}, {"content", "m2"}},
      {{"role", "user"}, {"content", "m3"}},
  }));
  auto last_two = s->messages(2);
  REQUIRE(last_two.size() == 2);
  CHECK(last_two[0]["content"] == "m2");
  CHECK(last_two[1]["content"] == "m3");
}

TEST_CASE("replace wipes then inserts") {
  auto s = agt::make_pg_session(pg_dsn(), fresh_session());
  s->append(json::array({{{"role", "user"}, {"content", "old"}}}));
  s->replace(json::array({{{"role", "user"}, {"content", "fresh"}}}));
  auto msgs = s->messages();
  REQUIRE(msgs.size() == 1);
  CHECK(msgs[0]["content"] == "fresh");
}

TEST_CASE("clear empties the session") {
  auto s = agt::make_pg_session(pg_dsn(), fresh_session());
  s->append(json::array({{{"role", "user"}, {"content", "hi"}}}));
  s->clear();
  CHECK(s->messages().empty());
}

TEST_CASE("compact(keep) drops oldest while preserving tool pairs") {
  auto s = agt::make_pg_session(pg_dsn(), fresh_session());
  // assistant tool-call + tool result + regular message × 3 — compact(3)
  // should drop the first pair without splitting them.
  s->append(json::array({
      {{"role", "assistant"}, {"content", nullptr}, {"call_id", "c1"}},
      {{"role", "tool"}, {"content", "r1"}, {"call_id", "c1"}},
      {{"role", "user"}, {"content", "u2"}},
      {{"role", "assistant"}, {"content", "a2"}},
      {{"role", "user"}, {"content", "u3"}},
  }));
  s->compact(3);
  auto kept = s->messages();
  REQUIRE(kept.size() == 3);
  CHECK(kept[0]["content"] == "u2");
  CHECK(kept[1]["content"] == "a2");
  CHECK(kept[2]["content"] == "u3");
}

TEST_CASE("two session_ids on the same DB stay isolated") {
  const auto id_a = fresh_session();
  const auto id_b = fresh_session();
  auto a = agt::make_pg_session(pg_dsn(), id_a);
  auto b = agt::make_pg_session(pg_dsn(), id_b);
  a->append(json::array({{{"role", "user"}, {"content", "for-a"}}}));
  b->append(json::array({{{"role", "user"}, {"content", "for-b"}}}));
  CHECK(a->messages().size() == 1);
  CHECK(a->messages()[0]["content"] == "for-a");
  CHECK(b->messages().size() == 1);
  CHECK(b->messages()[0]["content"] == "for-b");
}

} // TEST_SUITE
