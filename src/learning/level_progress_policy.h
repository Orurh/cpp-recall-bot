#pragma once

// Fixed-level manual practice counts only at the highest unlocked level.
// Replaying an earlier level still updates question statistics and reviews.
constexpr bool IsProgressionPractice(int selected_level, int unlocked_level) noexcept {
  return selected_level == unlocked_level;
}

// Use the actual card level in mixed practice, the selected level in manual
// practice. A lower-level answer must not change higher-level evidence.
constexpr bool CanApplyLevelEvidence(bool progression_practice, int selected_level, int current_level) noexcept {
  return progression_practice && selected_level >= current_level;
}
