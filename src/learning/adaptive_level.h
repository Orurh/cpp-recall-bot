#pragma once

#include <cstddef>

struct AdaptiveLevelState {
  int level = 3;
  int evidence = 0;
};

void ValidateAdaptiveLevelState(const AdaptiveLevelState& state);

[[nodiscard]]
AdaptiveLevelState ApplyAdaptiveEvidence(AdaptiveLevelState state, int question_level, bool correct,
                                         std::size_t attempts);
