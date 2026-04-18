#pragma once

#include <agt/json.hpp>

namespace agt {

/// Conversation history storage. Subclass for persistent backends.
class Session {
public:
  virtual ~Session() = default;

  /// Returns the last `count` messages, or all if count <= 0.
  virtual Json messages(int count = 0) const = 0;
  /// Appends messages to the history.
  virtual void append(const Json &messages) = 0;
  /// Atomically replaces the entire history.
  virtual void replace(const Json &messages) = 0;
  virtual void clear() noexcept = 0;

  /// Drop oldest messages, keeping the last `keep` messages.
  /// Must not split tool call / tool result pairs.
  virtual void compact(int keep) = 0;
};

/// In-memory session, lost when the object is destroyed.
class MemorySession : public Session {
  Json messages_ = Json::array();

public:
  Json messages(int count = 0) const override;
  void append(const Json &messages) override;
  void replace(const Json &messages) override;
  void clear() noexcept override;
  void compact(int keep) override;
};

/// Creates a SQLite-backed session. Messages persist in the file at \p db_path
/// under the given \p session_id; survives process restart.
std::shared_ptr<Session> make_sqlite_session(const std::string &db_path,
                                              const std::string &session_id);

/// Creates a Postgres-backed session. \p dsn is a libpq connection string
/// (`postgresql://user:pass@host/db` or equivalent key=value form). The
/// `messages` table is created on first use; many processes can share the
/// same Postgres without external coordination. Same interface as the SQLite
/// variant — pick whichever fits your deployment.
std::shared_ptr<Session> make_pg_session(const std::string &dsn,
                                          const std::string &session_id);

} // namespace agt
