#include "miniapp/telegram_share.h"

#include "bot/telegram_bot.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <optional>
#include <semaphore>
#include <stdexcept>
#include <string>
#include <utility>

namespace {

constexpr auto kCooldown = std::chrono::seconds(5);
constexpr auto kPrepareTimeout = std::chrono::seconds(4);
constexpr std::int64_t kMaxSafeInteger = 9'007'199'254'740'991;

struct PreparationPermit {
  std::binary_semaphore& available;
  ~PreparationPermit() { available.release(); }
};

bool ValidId(const boost::json::value* value) {
  if (value == nullptr || !value->is_string())
    return false;
  const auto& id = value->as_string();
  // Telegram defines an opaque string, not a particular base64 encoding.
  return !id.empty() && id.size() <= 512 &&
         std::ranges::all_of(id, [](unsigned char ch) { return ch > 32 && ch < 127; });
}

std::optional<std::int64_t> Expiration(const boost::json::value* value, std::chrono::system_clock::time_point now) {
  if (value == nullptr)
    return std::nullopt;
  std::int64_t expiration = 0;
  if (value->is_int64()) {
    expiration = value->as_int64();
  } else if (value->is_uint64() && value->as_uint64() <= static_cast<std::uint64_t>(kMaxSafeInteger)) {
    expiration = static_cast<std::int64_t>(value->as_uint64());
  } else {
    return std::nullopt;
  }
  const auto seconds = std::chrono::duration_cast<std::chrono::seconds>(now.time_since_epoch()).count();
  if (expiration <= seconds || expiration > kMaxSafeInteger)
    return std::nullopt;
  return expiration;
}

}  // namespace

TelegramShare::TelegramShare(std::string bot_token)
    : TelegramShare([token = std::move(bot_token)](std::string_view method, const boost::json::object& params,
                                                   const std::stop_token& stop_token) {
        // Never share a curl easy handle between HTTP workers or the polling bot.
        TelegramBot bot(token, kPrepareTimeout);
        return bot.Call(method, params, stop_token);
      }) {}

TelegramShare::TelegramShare(Transport transport, Clock clock)
    : transport_(std::move(transport)), clock_(std::move(clock)) {
  if (!transport_ || !clock_)
    throw std::invalid_argument("Telegram sharing requires a transport and clock");
}

bool TelegramShare::AdmitUser(std::int64_t user_id, std::chrono::steady_clock::time_point now) {
  std::lock_guard lock(cooldown_mutex_);
  std::erase_if(last_prepared_, [now](const auto& entry) { return now - entry.second >= kCooldown; });
  if (last_prepared_.contains(user_id))
    return false;
  if (last_prepared_.size() >= kMaxTrackedUsers) {
    const auto oldest = std::ranges::min_element(
        last_prepared_, [](const auto& lhs, const auto& rhs) { return lhs.second < rhs.second; });
    last_prepared_.erase(oldest);
  }
  last_prepared_.emplace(user_id, now);
  return true;
}

std::optional<TelegramShare::PreparedMessage> TelegramShare::Prepare(std::int64_t authenticated_user_id,
                                                                     const boost::json::object& validated_share,
                                                                     const std::stop_token& stop_token) noexcept {
  try {
    if (authenticated_user_id <= 0 || stop_token.stop_requested())
      return std::nullopt;
    const auto* url = validated_share.if_contains("url");
    const auto* text = validated_share.if_contains("text");
    if (url == nullptr || !url->is_string() || url->as_string().empty() || text == nullptr || !text->is_string() ||
        text->as_string().empty())
      return std::nullopt;
    std::string message(url->as_string());
    message += "\n\n";
    message.append(text->as_string().data(), text->as_string().size());
    // Conservative byte cap also stays within Telegram's 4096-character limit.
    // A future larger card retains the plain link instead of sending truncated code.
    if (message.size() > 4096 || !available_.try_acquire())
      return std::nullopt;
    const PreparationPermit permit{available_};
    if (!AdmitUser(authenticated_user_id, clock_().steady) || stop_token.stop_requested())
      return std::nullopt;
    const boost::json::object article{
        {"type", "article"},
        {"id", "cpp-recall-challenge"},
        {"title", "Задание C++ Recall"},
        {"input_message_content",
         boost::json::object{{"message_text", std::move(message)},
                             {"link_preview_options", boost::json::object{{"is_disabled", true}}}}}};
    const boost::json::object params{{"user_id", authenticated_user_id}, {"result", article},
                                     {"allow_user_chats", true},         {"allow_bot_chats", false},
                                     {"allow_group_chats", true},        {"allow_channel_chats", true}};
    const auto response = transport_("savePreparedInlineMessage", params, stop_token);
    if (stop_token.stop_requested() || !response.is_object())
      return std::nullopt;
    const auto& object = response.as_object();
    const auto* id = object.if_contains("id");
    const auto expiration = Expiration(object.if_contains("expiration_date"), clock_().system);
    if (!ValidId(id) || !expiration)
      return std::nullopt;
    return PreparedMessage{std::string(id->as_string()), *expiration};
  } catch (...) {
    // Do not expose Telegram error descriptions, tokens, or transport internals.
    // Sharing remains optional; the already-validated URL/text are unaffected.
    return std::nullopt;
  }
}
