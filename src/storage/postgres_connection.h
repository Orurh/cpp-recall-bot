#pragma once

#include "storage/database_timeouts.h"

#include <libpq-fe.h>

#include <chrono>
#include <stop_token>
#include <string>
#include <vector>

// Exclusively leased; never replays in-flight SQL, including COMMIT.
class PostgresConnection {
 public:
  PostgresConnection(std::string database_url, DatabaseTimeouts timeouts, std::stop_token stop_token = {});
  ~PostgresConnection();
  PostgresConnection(const PostgresConnection&) = delete;
  PostgresConnection& operator=(const PostgresConnection&) = delete;

  // Caller owns the result and must PQclear it. nullptr is never returned.
  [[nodiscard]] PGresult* Execute(const char* sql, const std::vector<std::string>* params = nullptr,
                                  bool allow_reconnect = true,
                                  std::chrono::milliseconds budget = std::chrono::milliseconds::zero());
  [[nodiscard]] const std::string& ErrorMessage() const noexcept;
  void Rollback() noexcept;
  void SetMigrationMode(bool enabled);

 private:
  using Deadline = std::chrono::steady_clock::time_point;
  void Connect(Deadline deadline);
  void CheckDeadline(Deadline deadline, bool cleanup) const;
  short WaitSocket(short events, Deadline deadline, bool cleanup) const;
  [[nodiscard]] PGresult* Exchange(const char* sql, const std::vector<std::string>* params, Deadline deadline,
                                   bool cleanup = false);
  void Close() noexcept;

  std::string database_url_;
  DatabaseTimeouts timeouts_;
  std::stop_token stop_token_;
  PGconn* connection_ = nullptr;
  std::string error_;
  bool migration_ = false;
};
