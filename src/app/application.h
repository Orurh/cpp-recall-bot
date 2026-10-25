#pragma once

#include "bot/telegram_bot.h"
#include "bot/telegram_update_handler.h"
#include "miniapp/miniapp_http_server.h"
#include "quiz/question_bank.h"
#include "storage/attempt_repository.h"

#include <memory>
#include <stop_token>
#include <string>
#include <vector>

class Application {
 public:
  Application(std::string token, std::vector<Question> questions, std::string database_url);

  void Run(const std::stop_token& stop_token);

 private:
  void RunTelegram(const std::stop_token& stop_token);
  TelegramBot bot_;
  QuestionBank questions_;
  std::shared_ptr<DatabaseConnectionPool> database_pool_;
  AttemptRepository attempts_;
  TelegramUpdateHandler updates_;
  std::unique_ptr<MiniAppHttpServer> miniapp_;
};
