#pragma once

#include <boost/json.hpp>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <mutex>
#include <optional>
#include <semaphore>
#include <stop_token>
#include <string>
#include <string_view>
#include <unordered_map>

// Optional Telegram transport at the HTTP boundary. Its input must be the
// server-validated ShareChallenge result, never a client-supplied article.
class TelegramShare {
 public:
  struct PreparedMessage {
    std::string id;
    std::int64_t expiration_date;
  };

  struct Time {
    std::chrono::steady_clock::time_point steady;
    std::chrono::system_clock::time_point system;
  };

  using Transport =
      std::function<boost::json::value(std::string_view, const boost::json::object&, const std::stop_token&)>;
  using Clock = std::function<Time()>;
  static constexpr std::size_t kMaxTrackedUsers = 512;

  explicit TelegramShare(std::string bot_token);
  explicit TelegramShare(
      Transport transport,
      Clock clock = [] { return Time{std::chrono::steady_clock::now(), std::chrono::system_clock::now()}; });

  // Busy, cancelled, throttled and failed preparation all mean plain-link fallback.
  // There is no queue, no automatic retry and no message is sent by this method.
  [[nodiscard]] std::optional<PreparedMessage> Prepare(std::int64_t authenticated_user_id,
                                                       const boost::json::object& validated_share,
                                                       const std::stop_token& stop_token = {}) noexcept;

 private:
  bool AdmitUser(std::int64_t user_id, std::chrono::steady_clock::time_point now);

  Transport transport_;
  Clock clock_;
  std::binary_semaphore available_{1};
  std::mutex cooldown_mutex_;
  std::unordered_map<std::int64_t, std::chrono::steady_clock::time_point> last_prepared_;
};
