#pragma once

#include "storage/database_timeouts.h"

#include <cstddef>
#include <functional>
#include <memory>
#include <string>

class AttemptRepository;
class PostgresConnection;

class DatabaseConnectionPool {
 public:
  DatabaseConnectionPool(std::string database_url, std::size_t connection_count, DatabaseTimeouts timeouts = {});
  ~DatabaseConnectionPool();

  DatabaseConnectionPool(const DatabaseConnectionPool&) = delete;
  DatabaseConnectionPool& operator=(const DatabaseConnectionPool&) = delete;
  DatabaseConnectionPool(DatabaseConnectionPool&&) = delete;
  DatabaseConnectionPool& operator=(DatabaseConnectionPool&&) = delete;

  [[nodiscard]] std::size_t Size() const noexcept;
  [[nodiscard]] std::size_t Available() const;
  void RequestStop() noexcept;

  // Infrastructure-only sharing between independent repositories. The callback
  // owns one bounded exclusive lease; it must not retain the connection.
  void WithConnection(const std::function<void(PostgresConnection&)>& operation);

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;

  friend class AttemptRepository;
};
