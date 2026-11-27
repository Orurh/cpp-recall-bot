#include "telegram_update_handler.h"

#include "bot/card_renderer.h"

#include <openssl/rand.h>
#include <boost/json.hpp>

#include <cstdint>
#include <optional>
#include <stop_token>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

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

  const auto chat_id = chat.at("id").as_int64();
  const auto user_id = message.at("from").as_object().at("id").as_int64();
  if (chat_id != user_id)
    return;

  if (const auto* shared = message.if_contains("users_shared"); shared != nullptr && shared->is_object()) {
    const auto& selection = shared->as_object();
    const auto* request = selection.if_contains("request_id");
    const auto* users = selection.if_contains("users");
    if (request == nullptr || !request->is_int64() || users == nullptr || !users->is_array())
      return;
    const auto request_id = request->as_int64();
    if (request_id <= 0 || request_id > 2147483647 || users->as_array().size() > 10)
      return;
    std::vector<std::int64_t> selected;
    for (const auto& item : users->as_array()) {
      if (!item.is_object())
        return;
      const auto* id = item.as_object().if_contains("user_id");
      if (id == nullptr || !id->is_int64() || id->as_int64() <= 0)
        return;
      selected.push_back(id->as_int64());
    }
    const auto matches = attempts_.get().MatchFriendDiscovery(user_id, static_cast<int>(request_id), selected);
    std::string notice = "Найдено знакомых, разрешивших обнаружение: " + std::to_string(matches.size()) + ".";
    boost::json::object remove_keyboard{{"remove_keyboard", true}};
    boost::json::object response{{"chat_id", chat_id}, {"text", notice}, {"reply_markup", remove_keyboard}};
    bot_.get().Call("sendMessage", response, stop_token);
    for (const auto peer : matches) {
      // Use Telegram's native profile link; do not store the selected address book.
      const auto link = "tg://user?id=" + std::to_string(peer);
      boost::json::object button{{"text", "Открыть знакомого"}, {"url", link}};
      boost::json::array row{button};
      boost::json::array keyboard{row};
      boost::json::object markup{{"inline_keyboard", keyboard}};
      const auto hint = "Можно отправить знакомому ссылку приглашения из Recall.";
      boost::json::object found{{"chat_id", chat_id}, {"text", hint}, {"reply_markup", markup}};
      bot_.get().Call("sendMessage", found, stop_token);
    }
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

  if (*command == BotCommand::kStart && input == "/start find_friends") {
    unsigned int random = 0;
    if (RAND_bytes(reinterpret_cast<unsigned char*>(&random), sizeof(random)) != 1)
      return;
    const auto request_id = static_cast<int>(random % 2147483647U + 1U);
    try {
      attempts_.get().BeginFriendDiscovery(user_id, request_id);
    } catch (const std::invalid_argument&) {
      bot_.get().Call("sendMessage", {{"chat_id", chat_id}, {"text", "Подожди 30 секунд перед новым поиском."}},
                      stop_token);
      return;
    }
    boost::json::object criteria{{"request_id", request_id}, {"user_is_bot", false}, {"max_quantity", 10}};
    boost::json::object button{{"text", "Выбрать знакомых"}, {"request_users", criteria}};
    boost::json::array row{button};
    boost::json::array keyboard{row};
    boost::json::object markup{{"keyboard", keyboard}, {"resize_keyboard", true}, {"one_time_keyboard", true}};
    const auto hint = "Выбери до 10 знакомых. Проверим только их.";
    boost::json::object response{{"chat_id", chat_id}, {"text", hint}, {"reply_markup", markup}};
    bot_.get().Call("sendMessage", response, stop_token);
    return;
  }

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
