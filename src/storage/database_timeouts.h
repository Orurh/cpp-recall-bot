#pragma once

#include <chrono>

struct DatabaseTimeouts {
  std::chrono::milliseconds connect{3'000};
  std::chrono::milliseconds query{6'000};
  std::chrono::milliseconds statement{5'000};
  std::chrono::milliseconds lock{1'000};
  std::chrono::milliseconds acquire{2'000};
  std::chrono::milliseconds cleanup{500};
  std::chrono::milliseconds migration{120'000};
};
