#pragma once

#include <cstdint>

struct QuestionReviewState {
  bool due = true;
  int stage = 0;
  std::int64_t seconds_until_due = 0;
  std::int64_t correct_count = 0;
  std::int64_t failed_count = 0;
};
