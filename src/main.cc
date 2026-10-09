#include "app/application.h"
#include "logging/logging.h"
#include "runtime/signal_watcher.h"
#include "storage/question_loader.h"

#include <spdlog/spdlog.h>

#include <cstdlib>
#include <exception>

int main() {
  InitializeLogging();

  const char* token =
      std::getenv("TELEGRAM_BOT_TOKEN");

  if (token == nullptr ||
      *token == '\0') {
    spdlog::critical(
        "TELEGRAM_BOT_TOKEN is not set");

    return EXIT_FAILURE;
  }

  const char* database_url =
      std::getenv("DATABASE_URL");

  if (database_url == nullptr ||
      *database_url == '\0') {
    spdlog::critical(
        "DATABASE_URL is not set");

    return EXIT_FAILURE;
  }

  try {
    SignalWatcher signals;
    signals.Start();

    Application app(
        token,
        LoadQuestions(
            "content/questions"),
        database_url);

    app.Run(
        signals.GetStopToken());

  } catch (
      const std::exception& error) {
    spdlog::critical(
        "application terminated: {}",
        error.what());

    return EXIT_FAILURE;
  }

  return EXIT_SUCCESS;
}
