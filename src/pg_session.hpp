#pragma once

#include <agt/session.hpp>
#include <memory>
#include <mutex>
#include <pqxx/pqxx>
#include <string>

namespace agt {

/// Postgres-backed conversation session. Mirrors SqliteSession 1:1 in
/// behaviour — different backend, identical interface. Each session owns a
/// pqxx::connection guarded by a mutex (pqxx::connection isn't thread-safe).
class PgSession : public Session {
  std::unique_ptr<pqxx::connection> conn_;
  mutable std::mutex mutex_;
  std::string session_id_;

  void exec(const char* sql) const;

public:
  PgSession(const std::string& dsn, const std::string& session_id);
  ~PgSession() noexcept override = default;

  PgSession(const PgSession&) = delete;
  PgSession& operator=(const PgSession&) = delete;
  PgSession(PgSession&&) = delete;
  PgSession& operator=(PgSession&&) = delete;

  Json messages(int count = 0) const override;
  void append(const Json& messages) override;
  void replace(const Json& messages) override;
  void clear() noexcept override;
  void compact(int keep) override;
};

} // namespace agt
