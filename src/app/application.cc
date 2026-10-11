#include "application.h"

#include <boost/json.hpp>

#include <spdlog/spdlog.h>

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <exception>
#include <mutex>
#include <utility>

Application::Application(std::string token, std::vector<Question> questions, std::string database_url)
    : bot_(std::move(token)),
      questions_(std::move(questions)),
      quiz_(questions_),
      attempts_(std::move(database_url)),
      updates_(bot_, quiz_, attempts_) {}

void Application::Run(const std::stop_token& stop_token) {
  using namespace std::chrono_literals;

  std::int64_t offset = 0;

  auto retry_delay = 1s;

  constexpr auto kMaxRetryDelay = 30s;

  std::mutex wait_mutex;
  std::condition_variable_any wait_cv;

  try {
    bot_.Call("setMyCommands",
              {
                  {"commands",
                   boost::json::array{
                       boost::json::object{
                           {"command", "start"},
                           {"description", "Начать заново"},
                       },
                       boost::json::object{
                           {"command", "menu"},
                           {"description", "Открыть главное меню"},
                       },
                       boost::json::object{
                           {"command", "stop"},
                           {"description", "Остановить тренировку"},
                       },
                       boost::json::object{
                           {"command", "stats"},
                           {"description", "Показать прогресс"},
                       },
                   }},
              },
              stop_token);

  } catch (const std::exception& error) {
    if (stop_token.stop_requested()) {
      return;
    }

    spdlog::warn("failed to configure Telegram commands: {}", error.what());
  }

  spdlog::info("application started, questions={}", questions_.Size());

  while (!stop_token.stop_requested()) {
    try {
      const auto updates = bot_.Call("getUpdates",
                                     {
                                         {"offset", offset},
                                         {"timeout", 25},
                                         {
                                             "allowed_updates",
                                             boost::json::array{
                                                 "message",
                                                 "callback_query",
                                             },
                                         },
                                     },
                                     stop_token);

      retry_delay = 1s;

      for (const auto& item : updates.as_array()) {
        if (stop_token.stop_requested()) {
          break;
        }

        const auto& update = item.as_object();

        const auto update_id = update.at("update_id").as_int64();

        try {
          updates_.Handle(update, stop_token);
        } catch (const std::exception& error) {
          if (stop_token.stop_requested()) {
            throw;
          }

          spdlog::error(
              "update processing failed, "
              "update_id={}, error={}",
              update_id, error.what());
        }

        offset = update_id + 1;
      }

    } catch (const std::exception& error) {
      if (stop_token.stop_requested()) {
        break;
      }

      spdlog::warn(
          "telegram polling failed, "
          "retry_in={}s, error={}",
          retry_delay.count(), error.what());

      std::unique_lock lock(wait_mutex);

      wait_cv.wait_for(lock, stop_token, retry_delay, [] { return false; });

      if (stop_token.stop_requested()) {
        break;
      }

      retry_delay = std::min(retry_delay * 2, kMaxRetryDelay);
    }
  }

  updates_.FinishActiveSessions();

  spdlog::info("application stopped");
}
