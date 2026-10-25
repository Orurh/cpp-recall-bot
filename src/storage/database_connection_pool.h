#pragma once

#include <cstddef>
#include <memory>
#include <string>

class AttemptRepository;

class DatabaseConnectionPool {
 public:
  DatabaseConnectionPool(std::string database_url, std::size_t connection_count);
  ~DatabaseConnectionPool();

  DatabaseConnectionPool(const DatabaseConnectionPool&) = delete;
  DatabaseConnectionPool& operator=(const DatabaseConnectionPool&) = delete;
  DatabaseConnectionPool(DatabaseConnectionPool&&) = delete;
  DatabaseConnectionPool& operator=(DatabaseConnectionPool&&) = delete;

  [[nodiscard]] std::size_t Size() const noexcept;
  [[nodiscard]] std::size_t Available() const;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;

  friend class AttemptRepository;
};
