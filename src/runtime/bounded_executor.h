#pragma once

#include <cstddef>
#include <functional>
#include <memory>

class BoundedExecutor {
 public:
  using Task = std::move_only_function<void()>;

  BoundedExecutor(std::size_t worker_count, std::size_t max_pending_tasks);
  ~BoundedExecutor();

  BoundedExecutor(const BoundedExecutor&) = delete;
  BoundedExecutor& operator=(const BoundedExecutor&) = delete;

  [[nodiscard]]
  bool TrySubmit(Task task);

  void Stop();

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};
