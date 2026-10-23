#include "application.h"

#include <boost/json.hpp>

#include <spdlog/spdlog.h>

#include <algorithm>
#include <charconv>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>

namespace {

std::size_t DatabasePoolSize() {
  constexpr std::size_t kDefaultPoolSize = 5;
  const char* value = std::getenv("DATABASE_POOL_SIZE");
  if (value == nullptr || *value == '\0') {
    return kDefaultPoolSize;
  }
  const std::string_view text(value);
  std::size_t size = 0;
  const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), size);
  if (error != std::errc{} || end != text.data() + text.size() || size < 2 || size > 32) {
    throw std::invalid_argument("DATABASE_POOL_SIZE must be between 2 and 32");
  }
  return size;
}

}  // namespace

Application::Application(std::string token, std::vector<Question> questions, std::string database_url)
    : bot_(token),
      questions_(std::move(questions)),
      database_pool_(std::make_shared<DatabaseConnectionPool>(std::move(database_url), DatabasePoolSize())),
      attempts_(database_pool_),
      updates_(bot_, attempts_) {
  if (auto config = LoadMiniAppServerConfig()) {
    miniapp_ = std::make_unique<MiniAppHttpServer>(std::move(*config), std::move(token), questions_, database_pool_);
    updates_.SetMiniAppUrl(miniapp_->Config().public_url);
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
                           {"description", "Открыть Mini App"},
                       },
                       boost::json::object{
                           {"command", "menu"},
                           {"description", "Открыть Mini App"},
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

  if (miniapp_thread.joinable()) {
    miniapp_thread.request_stop();
    miniapp_thread.join();
  }

  spdlog::info("application stopped");
}
