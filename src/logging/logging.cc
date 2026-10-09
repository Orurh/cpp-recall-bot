#include "logging.h"

#include <spdlog/spdlog.h>

void InitializeLogging() {
  spdlog::set_pattern("[%Y-%m-%d %H:%M:%S.%e] [%^%l%$] %v");

  spdlog::set_level(spdlog::level::info);
  spdlog::flush_on(spdlog::level::warn);
}