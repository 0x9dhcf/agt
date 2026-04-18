#include "pg_session.hpp"

#include <cstdint>
#include <pqxx/pqxx>
#include <stdexcept>

namespace agt {

namespace {

/// JSON-encode a field value. For strings we store the raw text; for
/// objects/arrays we store the JSON dump.
std::string encode_field(const Json& value) {
  if (value.is_string()) {
    return value.get<std::string>();
  }
  return value.dump();
}

/// Build the row-to-message mapping used by every read path. Same shape as
/// SqliteSession::row_to_message().
Json row_to_message(const pqxx::row& r) {
  Json msg;
  msg["role"] = r.at(0).as<std::string>();

  if (r.at(1).is_null()) {
    msg["content"] = nullptr;
  } else {
    msg["content"] = r.at(1).as<std::string>();
  }

  if (!r.at(2).is_null()) {
    msg["call_id"] = r.at(2).as<std::string>();
  }

  if (!r.at(3).is_null()) {
    // `calls` is stored as JSONB; libpqxx gives us the text form. The caller
    // owns parsing it back into nlohmann::json.
    msg["calls"] = Json::parse(r.at(3).as<std::string>());
  }

  return msg;
}

void insert_message(pqxx::work& txn, const std::string& session_id, const Json& msg) {
  const std::string role = msg.value("role", "");

  pqxx::params params;
  params.append(session_id);
  params.append(role);

  auto ci = msg.find("content");
  if (ci == msg.end() || ci->is_null()) {
    params.append();
  } else {
    params.append(encode_field(*ci));
  }

  auto cid = msg.find("call_id");
  if (cid == msg.end() || cid->is_null()) {
    params.append();
  } else {
    params.append(encode_field(*cid));
  }

  auto calls = msg.find("calls");
  if (calls == msg.end() || calls->is_null()) {
    params.append();
  } else {
    params.append(calls->dump());
  }

  txn.exec(
      "INSERT INTO messages (session_id, role, content, call_id, calls) "
      "VALUES ($1, $2, $3, $4, $5::jsonb)",
      params);
}

} // namespace

PgSession::PgSession(const std::string& dsn, const std::string& session_id)
    : conn_(std::make_unique<pqxx::connection>(dsn)), session_id_(session_id) {
  // Bootstrap: a single messages table shared by every session on this DB.
  // Designed to match SqliteSession's schema shape so the two backends stay
  // swappable without application changes.
  pqxx::work txn(*conn_);
  txn.exec(R"(
CREATE TABLE IF NOT EXISTS messages (
  session_id TEXT NOT NULL,
  seq        BIGSERIAL PRIMARY KEY,
  role       TEXT NOT NULL,
  content    TEXT,
  call_id    TEXT,
  calls      JSONB
)
)");
  txn.exec("CREATE INDEX IF NOT EXISTS idx_messages_session ON messages(session_id, seq)");
  txn.commit();
}

void PgSession::exec(const char* sql) const {
  std::lock_guard lock(mutex_);
  pqxx::work txn(*conn_);
  txn.exec(sql);
  txn.commit();
}

Json PgSession::messages(int count) const {
  std::lock_guard lock(mutex_);
  pqxx::work txn(*conn_);

  pqxx::result rows;
  if (count > 0) {
    rows = txn.exec(
        "SELECT role, content, call_id, calls FROM messages "
        "WHERE session_id = $1 AND seq IN ("
        "  SELECT seq FROM messages WHERE session_id = $1 "
        "  ORDER BY seq DESC LIMIT $2"
        ") ORDER BY seq",
        pqxx::params(session_id_, count));
  } else {
    rows = txn.exec(
        "SELECT role, content, call_id, calls FROM messages "
        "WHERE session_id = $1 ORDER BY seq",
        pqxx::params(session_id_));
  }
  txn.commit();

  Json arr = Json::array();
  for (const auto& row : rows) {
    arr.push_back(row_to_message(row));
  }
  return arr;
}

void PgSession::append(const Json& messages) {
  std::lock_guard lock(mutex_);
  pqxx::work txn(*conn_);
  for (const auto& msg : messages) {
    insert_message(txn, session_id_, msg);
  }
  txn.commit();
}

void PgSession::replace(const Json& messages) {
  std::lock_guard lock(mutex_);
  pqxx::work txn(*conn_);
  txn.exec("DELETE FROM messages WHERE session_id = $1", pqxx::params(session_id_));
  for (const auto& msg : messages) {
    insert_message(txn, session_id_, msg);
  }
  txn.commit();
}

void PgSession::clear() noexcept {
  try {
    std::lock_guard lock(mutex_);
    pqxx::work txn(*conn_);
    txn.exec("DELETE FROM messages WHERE session_id = $1", pqxx::params(session_id_));
    txn.commit();
  } catch (...) {
    // Matches SqliteSession::clear — swallow exceptions since destructors
    // (noexcept) may call this indirectly.
  }
}

void PgSession::compact(int keep) {
  if (keep <= 0) {
    return;
  }

  std::lock_guard lock(mutex_);
  pqxx::work txn(*conn_);

  const auto total_rows = txn.exec(
      "SELECT COUNT(*) FROM messages WHERE session_id = $1",
      pqxx::params(session_id_));
  const int total = total_rows.empty() ? 0 : total_rows[0].at(0).as<int>();

  if (keep >= total) {
    txn.commit();
    return;
  }

  const int to_drop = total - keep;

  // Walk forward from the oldest message; pick a cutoff seq that doesn't
  // split a tool-call / tool-result pair (same logic as the SQLite variant).
  const auto rows = txn.exec(
      "SELECT seq, role FROM messages WHERE session_id = $1 ORDER BY seq",
      pqxx::params(session_id_));

  std::int64_t cutoff_seq = 0;
  int idx = 0;
  for (const auto& row : rows) {
    const std::int64_t seq = row.at(0).as<std::int64_t>();
    const std::string role = row.at(1).as<std::string>();

    if (idx < to_drop) {
      cutoff_seq = seq + 1;
    } else if (idx == to_drop && role == "tool") {
      cutoff_seq = seq + 1;
    } else if (idx > to_drop && role == "tool" && cutoff_seq == seq) {
      cutoff_seq = seq + 1;
    } else if (idx >= to_drop) {
      break;
    }
    ++idx;
  }

  if (cutoff_seq > 0) {
    txn.exec(
        "DELETE FROM messages WHERE session_id = $1 AND seq < $2",
        pqxx::params(session_id_, cutoff_seq));
  }
  txn.commit();
}

std::shared_ptr<Session> make_pg_session(const std::string& dsn,
                                          const std::string& session_id) {
  return std::make_shared<PgSession>(dsn, session_id);
}

} // namespace agt
