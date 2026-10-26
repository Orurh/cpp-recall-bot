#pragma once

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <stdexcept>

// A mistake limits the interval, not the accumulated evidence or XP.
inline constexpr int kMaximumRepairReviewStage = 1;

struct ReviewSchedule {
  int stage = 0;
  std::chrono::minutes delay{};
};

[[nodiscard]] inline ReviewSchedule PlanQuestionReview(int previous_stage, bool correct, std::size_t attempts,
                                                       bool assisted) {
  if (previous_stage < 0 || previous_stage > 5 || attempts == 0) {
    throw std::invalid_argument("invalid review stage or attempt count");
  }
  if (!correct) {
    return {std::min(previous_stage, kMaximumRepairReviewStage), std::chrono::minutes{10}};
  }
  if (attempts > 1) {
    return {std::min(previous_stage, kMaximumRepairReviewStage), std::chrono::days{1}};
  }
  if (assisted) {
    return {previous_stage, std::chrono::days{1}};
  }
  constexpr std::array delays{std::chrono::days{1}, std::chrono::days{3}, std::chrono::days{7}, std::chrono::days{14},
                              std::chrono::days{30}};
  const int stage = std::min(previous_stage + 1, 5);
  return {stage, delays.at(static_cast<std::size_t>(stage - 1))};
}
