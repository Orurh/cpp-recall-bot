#include "application.h"

#include <boost/json.hpp>

#include <spdlog/spdlog.h>

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <exception>
#include <memory>
#include <mutex>
#include <thread>
#include <utility>

Application::Application(std::string token, std::vector<Question> questions, std::string database_url)
    : bot_(token),
      questions_(std::move(questions)),
      quiz_(questions_),
      attempts_(database_url),
      updates_(bot_, quiz_, attempts_) {
  if (auto config = LoadMiniAppServerConfig()) {
    miniapp_ =
        std::make_unique<MiniAppHttpServer>(std::move(*config), std::move(token), questions_, std::move(database_url));
  }
}

void Application::Run(const std::stop_token& stop_token) {
  using namespace std::chrono_literals;

  std::int64_t offset = 0;

  auto retry_delay = 1s;

  constexpr auto kMaxRetryDelay = 30s;

  std::mutex wait_mutex;
  std::condition_variable_any wait_cv;

  std::jthread miniapp_thread;
  if (miniapp_) {
    miniapp_thread = std::jthread([this](const std::stop_token& server_stop) {
      try {
        miniapp_->Run(server_stop);
      } catch (const std::exception& error) {
        spdlog::error("Mini App server stopped: {}", error.what());
      }
    });
  }

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

  if (miniapp_) {
    try {
      bot_.Call("setChatMenuButton",
                {{"menu_button",
                  boost::json::object{
                      {"type", "web_app"},
                      {"text", "Тренироваться"},
                      {"web_app", boost::json::object{{"url", miniapp_->Config().public_url}}},
                  }}},
                stop_token);
    } catch (const std::exception& error) {
      if (stop_token.stop_requested())
        return;
      spdlog::warn("failed to configure Mini App menu button: {}", error.what());
    }
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

  if (miniapp_thread.joinable()) {
    miniapp_thread.request_stop();
    miniapp_thread.join();
  }

  spdlog::info("application stopped");
}
