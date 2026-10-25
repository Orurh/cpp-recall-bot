#pragma once

#include <cstddef>
#include <functional>
#include <memory>

class BoundedExecutor {
 public:
  using Task = std::move_only_function<void()>;

  struct Statistics {
    std::size_t workers = 0;
    std::size_t active = 0;
    std::size_t pending = 0;
    std::size_t pending_limit = 0;
    std::size_t rejected = 0;
  };

  BoundedExecutor(std::size_t worker_count, std::size_t max_pending_tasks);
  ~BoundedExecutor();

  BoundedExecutor(const BoundedExecutor&) = delete;
  BoundedExecutor& operator=(const BoundedExecutor&) = delete;

  [[nodiscard]]
  bool TrySubmit(Task task);

  [[nodiscard]] Statistics Snapshot() const;

  void Stop();

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};
