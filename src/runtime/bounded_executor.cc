#include "runtime/bounded_executor.h"

#include <condition_variable>
#include <deque>
#include <mutex>
#include <stdexcept>
#include <stop_token>
#include <thread>
#include <utility>
#include <vector>

struct BoundedExecutor::Impl {
  Impl(std::size_t worker_count, std::size_t pending_limit) : max_pending_tasks(pending_limit) {
    if (worker_count == 0 || max_pending_tasks == 0) {
      throw std::invalid_argument("bounded executor sizes must be positive");
    }

    workers.reserve(worker_count);
    for (std::size_t index = 0; index < worker_count; ++index) {
      workers.emplace_back([this](const std::stop_token& stop_token) { Run(stop_token); });
    }
  }

  void Run(const std::stop_token& stop_token) {
    while (!stop_token.stop_requested()) {
      Task task;
      {
        std::unique_lock lock(mutex);
        condition.wait(lock, stop_token, [this] { return stopping || !tasks.empty(); });
        if (stop_token.stop_requested() || stopping) {
          return;
        }
        task = std::move(tasks.front());
        tasks.pop_front();
      }

      try {
        task();
      } catch (...) {
        // A failed connection must not terminate a worker and reduce server capacity.
      }
    }
  }

  bool TrySubmit(Task task) {
    if (!task) {
      throw std::invalid_argument("bounded executor task must not be empty");
    }

    std::scoped_lock lock(mutex);
    if (stopping || tasks.size() >= max_pending_tasks) {
      return false;
    }
    tasks.push_back(std::move(task));
    condition.notify_one();
    return true;
  }

  void Stop() {
    {
      std::scoped_lock lock(mutex);
      if (stopping) {
        return;
      }
      stopping = true;
      tasks.clear();
      for (auto& worker : workers) {
        worker.request_stop();
      }
    }
    condition.notify_all();
    for (auto& worker : workers) {
      if (worker.joinable()) {
        worker.join();
      }
    }
    workers.clear();
  }

  const std::size_t max_pending_tasks;
  std::mutex mutex;
  std::condition_variable_any condition;
  std::deque<Task> tasks;
  std::vector<std::jthread> workers;
  bool stopping = false;
};

BoundedExecutor::BoundedExecutor(std::size_t worker_count, std::size_t max_pending_tasks)
    : impl_(std::make_unique<Impl>(worker_count, max_pending_tasks)) {}

BoundedExecutor::~BoundedExecutor() {
  if (impl_) {
    impl_->Stop();
  }
}

bool BoundedExecutor::TrySubmit(Task task) {
  return impl_->TrySubmit(std::move(task));
}

void BoundedExecutor::Stop() {
  impl_->Stop();
}
