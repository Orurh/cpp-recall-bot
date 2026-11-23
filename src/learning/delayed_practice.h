#pragma once

#include <string_view>

// Kept in sync with content/transfer_pilots/raii_state_restore.json by tests.
// These are practice recommendation windows, not mastery thresholds.
inline constexpr std::string_view kDelayedPracticeId = "raii_state_restore/v1";
inline constexpr std::string_view kDelayedPreparationId = "vector_raii_early_return";
inline constexpr std::string_view kDelayedPreparationRevision =
    "51faaff2fd8a259b9b577c01349dcd01e9f277f424d1c2edf12c47fc32ba1522";
inline constexpr std::string_view kDelayedControlId = "raii_state_rollback_commit";
inline constexpr std::string_view kDelayedControlRevision =
    "674e69a7d8221b155ebb5a01665610d90ab3462d80ef2b9bf1068e606a27e3ad";
inline constexpr int kDelayedFirstPauseHours = 72;
inline constexpr int kDelayedFamiliarPauseHours = 336;

struct DelayedPracticeState {
  bool ready = false;
  bool familiar = false;
};
