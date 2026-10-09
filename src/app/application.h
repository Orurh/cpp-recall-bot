#pragma once

#include "app/quiz_session_service.h"
#include "bot/telegram_bot.h"
#include "bot/telegram_update_handler.h"
#include "quiz/question_bank.h"
#include "storage/attempt_repository.h"

#include <stop_token>
#include <string>
#include <vector>

class Application {
 public:
  Application(
      std::string token,
      std::vector<Question> questions,
      std::string database_url);

  void Run(
      const std::stop_token& stop_token);

 private:
  TelegramBot bot_;
  QuestionBank questions_;
  QuizSessionService quiz_;
  AttemptRepository attempts_;
  TelegramUpdateHandler updates_;
};
