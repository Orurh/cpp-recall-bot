#include "adaptive_level.h"

#include <algorithm>
#include <cstddef>
#include <stdexcept>

namespace {

constexpr int kMinLevel = 1;
constexpr int kMaxLevel = 10;
constexpr int kPromotionThreshold = 4;
constexpr int kDemotionThreshold = -4;
constexpr std::size_t kMaximumTrackedAttempts = 1'000;

int EvidenceDelta(const AdaptiveLevelState& state, int question_level, bool correct, std::size_t attempts) {
  if (correct) {
    if (question_level < state.level) {
      return 0;
    }

    if (attempts == 1) {
      return 2;
    }

    if (attempts == 2 || question_level > state.level) {
      return 0;
    }

    return -2;
  }

  throw std::invalid_argument("an incorrect answer cannot complete a learning card");
}

}  // namespace

void ValidateAdaptiveLevelState(const AdaptiveLevelState& state) {
  if (state.level < kMinLevel || state.level > kMaxLevel) {
    throw std::invalid_argument("adaptive level must be between 1 and 10");
  }

  if (state.evidence <= kDemotionThreshold || state.evidence >= kPromotionThreshold) {
    throw std::invalid_argument("adaptive evidence must be between -3 and 3");
  }
}

AdaptiveLevelState ApplyAdaptiveEvidence(AdaptiveLevelState state, int question_level, bool correct,
                                         std::size_t attempts) {
  ValidateAdaptiveLevelState(state);

  if (question_level < kMinLevel || question_level > kMaxLevel) {
    throw std::invalid_argument("question level must be between 1 and 10");
  }

  if (attempts < 1 || attempts > kMaximumTrackedAttempts) {
    throw std::invalid_argument("attempt count is outside its allowed range");
  }

  state.evidence += EvidenceDelta(state, question_level, correct, attempts);

  if (state.evidence >= kPromotionThreshold) {
    state.level = std::min(state.level + 1, kMaxLevel);
    state.evidence = 0;
  } else if (state.evidence <= kDemotionThreshold) {
    state.level = std::max(state.level - 1, kMinLevel);
    state.evidence = 0;
  }

  return state;
}
