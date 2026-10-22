#pragma once

// Only practice at the highest unlocked level can move the adaptive level.
// Replaying an earlier level still updates question statistics and reviews.
constexpr bool IsProgressionPractice(int selected_level, int unlocked_level) noexcept {
  return selected_level == unlocked_level;
}

// Once a fixed-level session raises the user above the practiced level, later
// answers in that same session must not move the adaptive state again.
constexpr bool CanApplyLevelEvidence(bool progression_practice, int selected_level, int current_level) noexcept {
  return progression_practice && selected_level >= current_level;
}
