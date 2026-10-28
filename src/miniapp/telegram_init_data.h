#pragma once

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

struct TelegramMiniAppUser {
  std::int64_t id = 0;
  std::string first_name;
  std::string last_name;
  std::string username;
  std::string language_code;
};

struct TelegramInitData {
  TelegramMiniAppUser user;
  std::int64_t auth_date = 0;
  std::optional<std::string> query_id;
  std::optional<std::string> start_param = std::nullopt;
};

[[nodiscard]]
std::optional<TelegramInitData> ValidateTelegramInitData(
    std::string_view raw_init_data, std::string_view bot_token,
    std::chrono::system_clock::time_point now = std::chrono::system_clock::now(),
    std::chrono::seconds max_age = std::chrono::hours(24));
