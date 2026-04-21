#include "sqlite_session.hpp"
#include <sqlite3.h>
#include <stdexcept>

namespace agt {

// RAII wrapper for sqlite3_stmt.
struct StmtGuard {
  sqlite3_stmt* s = nullptr;

  StmtGuard() = default;
  ~StmtGuard() {
    if (s)
      sqlite3_finalize(s);
  }

  StmtGuard(StmtGuard&) = delete;
  StmtGuard& operator=(StmtGuard&) = delete;

  StmtGuard(StmtGuard&&) = delete;
  StmtGuard& operator=(StmtGuard&&) = delete;
};

SqliteSession::SqliteSession(const std::string& db_path, const std::string& session_id)
    : session_id_(session_id) {
  if (sqlite3_open(db_path.c_str(), &db_) != SQLITE_OK) {
    std::string err = sqlite3_errmsg(db_);
    sqlite3_close(db_);
    throw std::runtime_error("sqlite_session: failed to open DB: " + err);
  }
  exec("PRAGMA journal_mode=WAL");
  // Wait up to 5s on a lock instead of surfacing SQLITE_BUSY to the caller.
  // Multiple processes / threads hitting the same file (Mission Control runs
  // one SqliteSession per detached mission thread) would otherwise crash on
  // the second concurrent write to the messages table.
  exec("PRAGMA busy_timeout=5000");
  exec("CREATE TABLE IF NOT EXISTS messages ("
       "  session_id TEXT NOT NULL,"
       "  seq INTEGER PRIMARY KEY AUTOINCREMENT,"
       "  role TEXT NOT NULL,"
       "  content TEXT,"
       "  call_id TEXT,"
       "  calls TEXT"
       ")");
  exec("CREATE INDEX IF NOT EXISTS idx_messages_session "
       "ON messages(session_id, seq)");
}

SqliteSession::~SqliteSession() noexcept {
  if (db_)
    sqlite3_close(db_);
}

void SqliteSession::exec(const char* sql) const {
  char* err = nullptr;
  if (sqlite3_exec(db_, sql, nullptr, nullptr, &err) != SQLITE_OK) {
    std::string msg = err ? err : "unknown error";
    sqlite3_free(err);
    throw std::runtime_error("sqlite_session: " + msg);
  }
}

sqlite3_stmt* SqliteSession::prepare(const char* sql) const {
  sqlite3_stmt* s = nullptr;
  if (sqlite3_prepare_v2(db_, sql, -1, &s, nullptr) != SQLITE_OK)
    throw std::runtime_error(std::string("sqlite_session: prepare failed: ") + sqlite3_errmsg(db_));
  return s;
}

static void bind_text(sqlite3_stmt* s, int idx, const std::string& val) {
  sqlite3_bind_text(s, idx, val.c_str(), static_cast<int>(val.size()), SQLITE_TRANSIENT);
}

static void bind_nullable(sqlite3_stmt* s, int idx, const Json& obj, const char* key) {
  auto it = obj.find(key);
  if (it == obj.end() || it->is_null()) {
    sqlite3_bind_null(s, idx);
  } else if (it->is_string()) {
    auto v = it->get<std::string>();
    sqlite3_bind_text(s, idx, v.c_str(), static_cast<int>(v.size()), SQLITE_TRANSIENT);
  } else {
    auto v = it->dump();
    sqlite3_bind_text(s, idx, v.c_str(), static_cast<int>(v.size()), SQLITE_TRANSIENT);
  }
}

void SqliteSession::insert_message(sqlite3_stmt* s, const Json& msg) const {
  sqlite3_reset(s);
  sqlite3_clear_bindings(s);
  bind_text(s, 1, session_id_);
  bind_text(s, 2, msg.value("role", ""));
  bind_nullable(s, 3, msg, "content");
  bind_nullable(s, 4, msg, "call_id");

  auto cit = msg.find("calls");
  if (cit != msg.end() && !cit->is_null()) {
    auto v = cit->dump();
    sqlite3_bind_text(s, 5, v.c_str(), static_cast<int>(v.size()), SQLITE_TRANSIENT);
  } else {
    sqlite3_bind_null(s, 5);
  }

  if (sqlite3_step(s) != SQLITE_DONE)
    throw std::runtime_error(std::string("sqlite_session: insert failed: ") + sqlite3_errmsg(db_));
}

static Json row_to_message(sqlite3_stmt* s) {
  Json msg;
  msg["role"] = reinterpret_cast<const char*>(sqlite3_column_text(s, 0));

  if (sqlite3_column_type(s, 1) != SQLITE_NULL)
    msg["content"] = std::string(reinterpret_cast<const char*>(sqlite3_column_text(s, 1)));
  else
    msg["content"] = nullptr;

  if (sqlite3_column_type(s, 2) != SQLITE_NULL)
    msg["call_id"] = std::string(reinterpret_cast<const char*>(sqlite3_column_text(s, 2)));

  if (sqlite3_column_type(s, 3) != SQLITE_NULL) {
    auto raw = std::string(reinterpret_cast<const char*>(sqlite3_column_text(s, 3)));
    msg["calls"] = Json::parse(raw);
  }

  return msg;
}

Json SqliteSession::messages(int count) const {
  Json arr = Json::array();
  StmtGuard g;

  if (count > 0) {
    g.s = prepare("SELECT role, content, call_id, calls FROM messages "
                  "WHERE session_id = ?1 AND seq IN ("
                  "  SELECT seq FROM messages WHERE session_id = ?1 "
                  "  ORDER BY seq DESC LIMIT ?2"
                  ") ORDER BY seq");
    bind_text(g.s, 1, session_id_);
    sqlite3_bind_int(g.s, 2, count);
  } else {
    g.s = prepare("SELECT role, content, call_id, calls FROM messages "
                  "WHERE session_id = ?1 ORDER BY seq");
    bind_text(g.s, 1, session_id_);
  }

  while (sqlite3_step(g.s) == SQLITE_ROW)
    arr.push_back(row_to_message(g.s));

  return arr;
}

void SqliteSession::append(const Json& messages) {
  exec("BEGIN");
  StmtGuard g;
  g.s = prepare("INSERT INTO messages (session_id, role, content, call_id, calls) "
                "VALUES (?1, ?2, ?3, ?4, ?5)");

  for (const auto& msg : messages)
    insert_message(g.s, msg);

  exec("COMMIT");
}

void SqliteSession::replace(const Json& messages) {
  exec("BEGIN");

  {
    StmtGuard g;
    g.s = prepare("DELETE FROM messages WHERE session_id = ?1");
    bind_text(g.s, 1, session_id_);
    sqlite3_step(g.s);
  }

  StmtGuard g;
  g.s = prepare("INSERT INTO messages (session_id, role, content, call_id, calls) "
                "VALUES (?1, ?2, ?3, ?4, ?5)");

  for (const auto& msg : messages)
    insert_message(g.s, msg);

  exec("COMMIT");
}

void SqliteSession::clear() noexcept {
  try {
    StmtGuard g;
    g.s = prepare("DELETE FROM messages WHERE session_id = ?1");
    bind_text(g.s, 1, session_id_);
    sqlite3_step(g.s);
  } catch (...) {
  }
}

void SqliteSession::compact(int keep) {
  if (keep <= 0)
    return;

  // Get total count.
  int total = 0;
  {
    StmtGuard g;
    g.s = prepare("SELECT COUNT(*) FROM messages WHERE session_id = ?1");
    bind_text(g.s, 1, session_id_);
    if (sqlite3_step(g.s) == SQLITE_ROW)
      total = sqlite3_column_int(g.s, 0);
  }

  if (keep >= total)
    return;

  // Find the seq at the tentative cut point (size - keep).
  // Then walk forward past any tool results to avoid splitting pairs.
  int to_drop = total - keep;
  int64_t cutoff_seq = 0;
  {
    StmtGuard g;
    g.s = prepare("SELECT seq, role FROM messages WHERE session_id = ?1 "
                  "ORDER BY seq LIMIT ?2");
    bind_text(g.s, 1, session_id_);
    sqlite3_bind_int(g.s, 2, total); // fetch all to walk
    int idx = 0;
    int64_t last_seq = 0;
    while (sqlite3_step(g.s) == SQLITE_ROW) {
      int64_t seq = sqlite3_column_int64(g.s, 0);
      auto role = std::string(reinterpret_cast<const char*>(sqlite3_column_text(g.s, 1)));

      if (idx < to_drop) {
        cutoff_seq = seq + 1; // delete everything with seq < cutoff_seq
      } else if (idx == to_drop && role == "tool") {
        // We landed on a tool result — keep walking forward.
        cutoff_seq = seq + 1;
      } else if (idx > to_drop && role == "tool" && cutoff_seq == seq) {
        // Still on consecutive tool results right at boundary.
        cutoff_seq = seq + 1;
      } else if (idx >= to_drop) {
        break;
      }
      last_seq = seq;
      ++idx;
    }
    (void)last_seq;
  }

  if (cutoff_seq <= 0)
    return;

  StmtGuard g;
  g.s = prepare("DELETE FROM messages WHERE session_id = ?1 AND seq < ?2");
  bind_text(g.s, 1, session_id_);
  sqlite3_bind_int64(g.s, 2, cutoff_seq);
  sqlite3_step(g.s);
}

std::shared_ptr<Session> make_sqlite_session(const std::string& db_path,
                                             const std::string& session_id) {
  return std::make_shared<SqliteSession>(db_path, session_id);
}

} // namespace agt
