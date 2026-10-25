#pragma once

#include <boost/json.hpp>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <mutex>
#include <string>
#include <unordered_map>

#ifndef CPP_RECALL_BUILD_VERSION
#define CPP_RECALL_BUILD_VERSION "unknown"
#endif

class RuntimeMetrics {
 public:
  static RuntimeMetrics& Instance() {
    static RuntimeMetrics value;
    return value;
  }

  [[nodiscard]] std::string RequestId() { return std::to_string(started_at_) + "-" + std::to_string(++sequence_); }

  void Request(std::string_view route, unsigned int status, std::int64_t duration_ms) {
    std::scoped_lock lock(mutex_);
    auto& counters = routes_[std::string(route)];
    ++counters.requests;
    counters.errors += status >= 500 ? 1 : 0;
    counters.client_errors += status >= 400 && status < 500 ? 1 : 0;
    counters.total_ms += duration_ms;
  }

  void PollSuccess() { last_poll_success_ = EpochSeconds(); }
  void PollFailure() { ++poll_errors_; }

  [[nodiscard]] boost::json::object Snapshot() {
    std::scoped_lock lock(mutex_);
    boost::json::array routes;
    std::int64_t total = 0;
    std::int64_t errors = 0;
    for (const auto& [route, counters] : routes_) {
      total += counters.requests;
      errors += counters.errors;
      routes.emplace_back(boost::json::object{{"route", route},
                                              {"requests", counters.requests},
                                              {"server_errors", counters.errors},
                                              {"client_errors", counters.client_errors},
                                              {"total_duration_ms", counters.total_ms}});
    }
    return {{"version", CPP_RECALL_BUILD_VERSION},
            {"started_at", started_at_},
            {"uptime_seconds", EpochSeconds() - started_at_},
            {"http_requests", total},
            {"http_server_errors", errors},
            {"telegram_last_success", last_poll_success_.load()},
            {"telegram_errors", poll_errors_.load()},
            {"routes", std::move(routes)}};
  }

 private:
  static std::int64_t EpochSeconds() {
    return std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch())
        .count();
  }
  struct Counters {
    std::int64_t requests = 0;
    std::int64_t errors = 0;
    std::int64_t client_errors = 0;
    std::int64_t total_ms = 0;
  };
  const std::int64_t started_at_ = EpochSeconds();
  std::atomic<std::int64_t> sequence_{0};
  std::atomic<std::int64_t> last_poll_success_{0};
  std::atomic<std::int64_t> poll_errors_{0};
  std::mutex mutex_;
  std::unordered_map<std::string, Counters> routes_;
};
