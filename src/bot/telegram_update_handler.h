#pragma once

#include "bot/telegram_bot.h"
#include "storage/attempt_repository.h"

#include <boost/json/object.hpp>

#include <cstdint>
#include <functional>
#include <stop_token>
#include <string>
#include <string_view>

class TelegramUpdateHandler {
 public:
  TelegramUpdateHandler(TelegramBot& bot, AttemptRepository& attempts);

  void SetMiniAppUrl(std::string url);

  void Handle(const boost::json::object& update, const std::stop_token& stop_token);

 private:
  void HandleMessage(const boost::json::object& message, const std::stop_token& stop_token);

  void HandleLegacyCallback(const boost::json::object& callback, const std::stop_token& stop_token);

  void ShowMiniAppMenu(std::int64_t user_id, std::int64_t chat_id, std::string_view notice,
                       const std::stop_token& stop_token);

  void ShowProgressSummary(std::int64_t user_id, std::int64_t chat_id, const std::stop_token& stop_token);

  std::reference_wrapper<TelegramBot> bot_;
  std::reference_wrapper<AttemptRepository> attempts_;
  std::string mini_app_url_;
};
