#include "telegram_update_handler.h"

#include "bot/card_renderer.h"

#include <boost/json.hpp>

#include <cstdint>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>
#include <utility>

namespace {

enum class BotCommand : std::uint8_t {
  kStart,
  kMenu,
  kLegacyStop,
  kStats,
  kUnknown,
};

std::optional<BotCommand> ParseBotCommand(std::string_view input) {
  if (input.empty() || input.front() != '/') {
    return std::nullopt;
  }

  const auto end = input.find_first_of(" \t\r\n");
  auto token = input.substr(0, end);

  if (const auto at = token.find('@'); at != std::string_view::npos) {
    token = token.substr(0, at);
  }

  if (token == "/start") {
    return BotCommand::kStart;
  }
  if (token == "/menu") {
    return BotCommand::kMenu;
  }
  if (token == "/stop") {
    return BotCommand::kLegacyStop;
  }
  if (token == "/stats") {
    return BotCommand::kStats;
  }
  return BotCommand::kUnknown;
}

}  // namespace

TelegramUpdateHandler::TelegramUpdateHandler(TelegramBot& bot, AttemptRepository& attempts)
    : bot_(bot), attempts_(attempts) {}

void TelegramUpdateHandler::SetMiniAppUrl(std::string url) {
  mini_app_url_ = std::move(url);
}

void TelegramUpdateHandler::Handle(const boost::json::object& update, const std::stop_token& stop_token) {
  if (const auto* message = update.if_contains("message"); message != nullptr && message->is_object()) {
    HandleMessage(message->as_object(), stop_token);
    return;
  }

  if (const auto* callback = update.if_contains("callback_query"); callback != nullptr && callback->is_object()) {
    HandleLegacyCallback(callback->as_object(), stop_token);
  }
}

void TelegramUpdateHandler::HandleMessage(const boost::json::object& message, const std::stop_token& stop_token) {
  const auto& chat = message.at("chat").as_object();
  if (chat.at("type").as_string() != "private") {
    return;
  }

  const auto* text_value = message.if_contains("text");
  if (text_value == nullptr || !text_value->is_string()) {
    return;
  }

  const auto& text = text_value->as_string();
  const std::string_view input(text.data(), text.size());
  const auto command = ParseBotCommand(input);
  if (!command || *command == BotCommand::kUnknown) {
    return;
  }

  const auto chat_id = chat.at("id").as_int64();
  const auto user_id = message.at("from").as_object().at("id").as_int64();

  switch (*command) {
    case BotCommand::kStart:
      attempts_.get().RecordBotStart(user_id, message.at("message_id").as_int64());
      ShowMiniAppMenu(user_id, chat_id, {}, stop_token);
      return;
    case BotCommand::kMenu:
      ShowMiniAppMenu(user_id, chat_id, {}, stop_token);
      return;
    case BotCommand::kLegacyStop:
      ShowMiniAppMenu(user_id, chat_id, "Тренировки в чате больше не используются.", stop_token);
      return;
    case BotCommand::kStats:
      ShowProgressSummary(user_id, chat_id, stop_token);
      return;
    case BotCommand::kUnknown:
      return;
  }
}

void TelegramUpdateHandler::HandleLegacyCallback(const boost::json::object& callback,
                                                 const std::stop_token& stop_token) {
  const auto* callback_id = callback.if_contains("id");
  if (callback_id == nullptr || !callback_id->is_string()) {
    return;
  }

  bot_.get().Call("answerCallbackQuery",
                  {
                      {"callback_query_id", *callback_id},
                      {"text", "Тренировки перенесены в Mini App"},
                  },
                  stop_token);

  const auto* message_value = callback.if_contains("message");
  const auto* from_value = callback.if_contains("from");
  if (message_value == nullptr || !message_value->is_object() || from_value == nullptr || !from_value->is_object()) {
    return;
  }

  const auto& message = message_value->as_object();
  const auto& chat = message.at("chat").as_object();
  if (chat.at("type").as_string() != "private") {
    return;
  }

  const auto user_id = from_value->as_object().at("id").as_int64();
  const auto chat_id = chat.at("id").as_int64();
  attempts_.get().EnsureUser(user_id);

  auto request = BuildMiniAppMenuMessage(chat_id, mini_app_url_, "Чатовая тренировка отключена.");
  request["message_id"] = message.at("message_id");
  bot_.get().Call("editMessageText", request, stop_token);
}

void TelegramUpdateHandler::ShowMiniAppMenu(std::int64_t user_id, std::int64_t chat_id, std::string_view notice,
                                            const std::stop_token& stop_token) {
  attempts_.get().EnsureUser(user_id);
  bot_.get().Call("sendMessage", BuildMiniAppMenuMessage(chat_id, mini_app_url_, notice), stop_token);
}

void TelegramUpdateHandler::ShowProgressSummary(std::int64_t user_id, std::int64_t chat_id,
                                                const std::stop_token& stop_token) {
  const auto summary = attempts_.get().LoadProgressSummary(user_id);
  bot_.get().Call("sendMessage", BuildProgressSummaryMessage(chat_id, summary), stop_token);
}
