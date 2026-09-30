#include "bot/card_renderer.h"
#include "bot/telegram_bot.h"
#include "quiz/quiz_service.h"
#include "storage/question_loader.h"

#include <boost/json.hpp>

#include <cstdint>
#include <cstdlib>
#include <exception>
#include <iostream>
#include <string>
#include <string_view>

namespace {

std::string BuildAnswerText(const Question& question, std::string_view option_id, AnswerResult result) {
  std::string text = question.text;

  if (question.code) {
    text += "\n\n";
    text += *question.code;
  }

  text += "\n\nВаш ответ: ";

  for (const auto& option : question.options) {
    if (option.id == option_id) {
      text += option.text;
      break;
    }
  }

  text += result == AnswerResult::kCorrect ? "\n✅ Верно!" : "\n❌ Неверно.";

  text += "\n\n";
  text += question.explanation;

  return text;
}

void HandleMessage(TelegramBot& bot, const boost::json::object& message, const Question& question) {
  const auto* text = message.if_contains("text");
  if (text == nullptr || !text->is_string()) {
    return;
  }

  if (text->as_string() == "/start") {
    const auto chat_id = message.at("chat").as_object().at("id").as_int64();

    bot.Call("sendMessage", BuildQuestionMessage(chat_id, question));
  }
}

void HandleCallback(TelegramBot& bot, const boost::json::object& callback, const Question& question) {
  // Подтверждаем получение нажатия, чтобы убрать индикатор ожидания.
  bot.Call("answerCallbackQuery", {
                                      {"callback_query_id", callback.at("id")},
                                  });

  const auto* data_value = callback.if_contains("data");
  const auto* message_value = callback.if_contains("message");

  if (data_value == nullptr || !data_value->is_string() || message_value == nullptr || !message_value->is_object()) {
    return;
  }

  const auto& data = data_value->as_string();
  const std::string_view payload(data.data(), data.size());
  const std::string prefix = "answer:" + question.id + ":";

  if (!payload.starts_with(prefix)) {
    return;
  }

  const auto option_id = payload.substr(prefix.size());
  const auto result = CheckAnswer(question, option_id);

  if (result == AnswerResult::kUnknownOption) {
    return;
  }

  const auto& message = message_value->as_object();
  const auto chat_id = message.at("chat").as_object().at("id").as_int64();

  bot.Call("editMessageText", {
                                  {"chat_id", chat_id},
                                  {"message_id", message.at("message_id")},
                                  {"text", BuildAnswerText(question, option_id, result)},
                                  {"reply_markup",
                                   boost::json::object{
                                       {"inline_keyboard", boost::json::array{}},
                                   }},
                              });
}

void HandleUpdate(TelegramBot& bot, const boost::json::object& update, const Question& question) {
  if (const auto* message = update.if_contains("message"); message != nullptr && message->is_object()) {
    HandleMessage(bot, message->as_object(), question);
    return;
  }

  if (const auto* callback = update.if_contains("callback_query"); callback != nullptr && callback->is_object()) {
    HandleCallback(bot, callback->as_object(), question);
  }
}

void RunBot(TelegramBot& bot, const Question& question) {
  std::int64_t offset = 0;

  std::cout << "Бот запущен. Отправь /start в Telegram.\n";

  while (true) {
    const auto updates =
        bot.Call("getUpdates", {
                                   {"offset", offset},
                                   {"timeout", 25},
                                   {"allowed_updates", boost::json::array{"message", "callback_query"}},
                               });

    for (const auto& item : updates.as_array()) {
      const auto& update = item.as_object();
      const auto update_id = update.at("update_id").as_int64();

      try {
        HandleUpdate(bot, update, question);
      } catch (const std::exception& error) {
        // Ошибка одного события не останавливает обработку остальных.
        std::cerr << "Ошибка обработки update " << update_id << ": " << error.what() << '\n';
      }

      // Не возвращаемся бесконечно к событию, которое не удалось обработать.
      offset = update_id + 1;
    }
  }
}

}  // namespace

int main() {
  const char* token = std::getenv("TELEGRAM_BOT_TOKEN");

  if (token == nullptr || *token == '\0') {
    std::cerr << "Set TELEGRAM_BOT_TOKEN\n";
    return EXIT_FAILURE;
  }

  try {
    TelegramBot bot(token);
    const auto question = LoadQuestion("content/questions/vector_reserve_001.json");

    RunBot(bot, question);
  } catch (const std::exception& error) {
    std::cerr << "Ошибка запуска или получения обновлений: " << error.what() << '\n';
    return EXIT_FAILURE;
  }

  return EXIT_SUCCESS;
}