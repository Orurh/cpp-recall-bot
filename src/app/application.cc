#include "application.h"

#include "monitoring/runtime_metrics.h"

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
  constexpr std::size_t k_default_pool_size = 5;
  const char* value = std::getenv("DATABASE_POOL_SIZE");
  if (value == nullptr || *value == '\0') {
    return k_default_pool_size;
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
  std::stop_source shutdown;
  std::stop_callback external_stop(stop_token, [&] { shutdown.request_stop(); });
  std::exception_ptr server_failure;
  std::jthread miniapp_thread;
  if (miniapp_) {
    miniapp_thread = std::jthread([&](const std::stop_token& server_stop) {
      try {
        miniapp_->Run(server_stop);
        if (!server_stop.stop_requested()) {
          throw std::runtime_error("Mini App server returned unexpectedly");
        }
      } catch (...) {
        server_failure = std::current_exception();
        spdlog::error("event=miniapp_server outcome=failed category=server_terminated version={}",
                      CPP_RECALL_BUILD_VERSION);
        shutdown.request_stop();
      }
    });
  }
  std::stop_callback stop_server(shutdown.get_token(), [&] { miniapp_thread.request_stop(); });
  RunTelegram(shutdown.get_token());
  if (miniapp_thread.joinable()) {
    miniapp_thread.request_stop();
    miniapp_thread.join();
  }
  // Read only after join: the exception is written by the server thread.
  if (server_failure) {
    throw std::runtime_error("Mini App HTTP server terminated unexpectedly");
  }
}

void Application::RunTelegram(const std::stop_token& stop_token) {
  using namespace std::chrono_literals;

  std::int64_t offset = 0;

  auto retry_delay = 1s;

  constexpr auto k_max_retry_delay = 30s;

  std::mutex wait_mutex;
  std::condition_variable_any wait_cv;

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

    spdlog::warn("event=telegram_commands outcome=failed version={}", CPP_RECALL_BUILD_VERSION);
  }

  if (miniapp_) {
    try {
      bot_.Call("setChatMenuButton",
                {
                    {
                        "menu_button",
                        boost::json::object{
                            {"type", "web_app"},
                            {"text", "Тренироваться"},
                            {"web_app", boost::json::object{{"url", miniapp_->Config().public_url}}},
                        },
                    },
                },
                stop_token);
    } catch (const std::exception& error) {
      if (stop_token.stop_requested()) {
        return;
      }
      spdlog::warn("event=telegram_menu outcome=failed version={}", CPP_RECALL_BUILD_VERSION);
    }
  }

  spdlog::info("event=application outcome=started questions={} version={}", questions_.Size(),
               CPP_RECALL_BUILD_VERSION);

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
      RuntimeMetrics::Instance().PollSuccess();

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

          spdlog::error("event=telegram_update outcome=failed code=update_processing_error");
        }

        offset = update_id + 1;
      }

    } catch (const std::exception& error) {
      if (stop_token.stop_requested()) {
        break;
      }

      RuntimeMetrics::Instance().PollFailure();
      spdlog::warn("event=telegram_poll outcome=failed retry_seconds={}", retry_delay.count());

      std::unique_lock lock(wait_mutex);

      wait_cv.wait_for(lock, stop_token, retry_delay, [] { return false; });

      if (stop_token.stop_requested()) {
        break;
      }

      retry_delay = std::min(retry_delay * 2, k_max_retry_delay);
    }
  }

  spdlog::info("application stopped");
}
