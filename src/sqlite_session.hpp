#pragma once

#include <agt/session.hpp>
#include <sqlite3.h>
#include <string>

namespace agt {

class SqliteSession : public Session {
  sqlite3* db_ = nullptr;
  std::string session_id_;

  void exec(const char* sql) const;
  sqlite3_stmt* prepare(const char* sql) const;
  void insert_message(sqlite3_stmt* s, const Json& msg) const;

public:
  SqliteSession(const std::string& db_path, const std::string& session_id);
  ~SqliteSession() noexcept;

  SqliteSession(SqliteSession&) = delete;
  SqliteSession& operator=(SqliteSession&) = delete;

  SqliteSession(SqliteSession&&) = default;
  SqliteSession& operator=(SqliteSession&&) = default;

  Json messages(int count = 0) const override;
  void append(const Json& messages) override;
  void replace(const Json& messages) override;
  void clear() noexcept override;
  void compact(int keep) override;
};

} // namespace agt
