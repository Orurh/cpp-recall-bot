#include "card_renderer.h"

#include <boost/json.hpp>

#include <stdexcept>
#include <string>
#include <utility>

boost::json::object BuildQuestionMessage(std::int64_t chat_id, const Question& question) {
  std::string text = question.text;  // делаем копию, чтобы не изменять оригинал
  if (question.code) {               // если есть код
    text += "\n\n";                  // добавляем пустую строку
    text += *question.code;          // добавляем код
  }

  boost::json::array keyboard;  // создаем пустой массив

  for (const auto& option : question.options) {                             // добавляем кнопки
    std::string callback_data = "answer:" + question.id + ":" + option.id;  // формируем callback_data

    if (callback_data.size() > 64) {  // проверяем, что callback_data не превышает 64 байт
      throw std::invalid_argument("callback_data exceeds 64 bytes");  // если превышает, то выбрасываем исключение
    }

    boost::json::object button{
        // создаем кнопку
        {"text", option.text},             // добавляем текст
        {"callback_data", callback_data},  // добавляем callback_data
    };

    boost::json::array row;              // создаем пустой массив
    row.push_back(std::move(button));    // добавляем кнопку
    keyboard.push_back(std::move(row));  // добавляем массив
  }

  return {
      {"chat_id", chat_id},
      {"text", text},
      {
          "reply_markup",
          boost::json::object{
              {"inline_keyboard", std::move(keyboard)},
          },
      },
  };
}