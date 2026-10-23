#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <utility>

class KeyedMutex {
 public:
  class Guard {
   public:
    explicit Guard(std::shared_ptr<std::mutex> mutex) : mutex_(std::move(mutex)), lock_(*mutex_) {}

    Guard(const Guard&) = delete;
    Guard& operator=(const Guard&) = delete;
    Guard(Guard&&) noexcept = default;
    Guard& operator=(Guard&&) = delete;

   private:
    std::shared_ptr<std::mutex> mutex_;
    std::unique_lock<std::mutex> lock_;
  };

  [[nodiscard]] Guard Acquire(std::int64_t key) {
    std::shared_ptr<std::mutex> key_mutex;
    {
      std::scoped_lock lock(registry_mutex_);
      auto& stored = mutexes_[key];
      key_mutex = stored.lock();
      if (!key_mutex) {
        key_mutex = std::make_shared<std::mutex>();
        stored = key_mutex;
      }
      ++acquisition_count_;
      if (mutexes_.size() > 1024 && acquisition_count_ % 256 == 0) {
        std::erase_if(mutexes_, [](const auto& item) { return item.second.expired(); });
      }
    }
    return Guard(std::move(key_mutex));
  }

 private:
  std::mutex registry_mutex_;
  std::unordered_map<std::int64_t, std::weak_ptr<std::mutex>> mutexes_;
  std::size_t acquisition_count_ = 0;
};
