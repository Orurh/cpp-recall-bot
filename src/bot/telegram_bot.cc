#include "telegram_bot.h"

#include <boost/json.hpp>

#include <chrono>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

namespace {

constexpr std::string_view kApiBase = "https://api.telegram.org/bot";

void ValidateMethod(std::string_view method) {
  if (method.empty() || method.find_first_not_of("abcdefghijklmnopqrstuvwxyz"
                                                 "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
                                                 "0123456789_") != std::string_view::npos) {
    throw std::invalid_argument("Invalid Telegram method name");
  }
}

boost::json::value ParseResponse(long status, std::string_view body) {
  boost::system::error_code parse_error;

  auto document = boost::json::parse(body, parse_error);

  if (parse_error || !document.is_object()) {
    throw std::runtime_error("Telegram returned invalid JSON; HTTP " + std::to_string(status));
  }

  const auto& object = document.as_object();

  const auto* ok = object.if_contains("ok");

  if (status < 200 || status >= 300 || ok == nullptr || !ok->is_bool() || !ok->as_bool()) {
    std::string message = "Telegram API error; HTTP " + std::to_string(status);

    if (const auto* description = object.if_contains("description");
        description != nullptr && description->is_string()) {
      const auto& text = description->as_string();

      message += ": ";
      message.append(text.data(), text.size());
    }

    throw std::runtime_error(message);
  }

  return object.at("result");
}

}  // namespace

TelegramBot::TelegramBot(std::string token, std::chrono::milliseconds request_timeout)
    : http_(request_timeout), token_(std::move(token)) {
  if (token_.empty()) {
    throw std::invalid_argument("Telegram token is empty");
  }
}

boost::json::value TelegramBot::Call(std::string_view method, const boost::json::object& params,
                                     const std::stop_token& stop_token) {
  ValidateMethod(method);

  const std::string url = std::string(kApiBase) + token_ + "/" + std::string(method);

  const std::string body = boost::json::serialize(params);

  const auto response = http_.PostJson(url, body, stop_token);

  return ParseResponse(response.status_code, response.body);
}
