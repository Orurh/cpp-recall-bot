#pragma once

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

#include <boost/json/fwd.hpp>

struct AdaptiveFamilyEvidence {
  std::string id;
  bool application = false;
  bool operator==(const AdaptiveFamilyEvidence&) const = default;
};

struct AdaptivePromotionEvidence {
  // Zero is the legacy/default empty state, never evidence for a difficulty.
  int level = 0;
  std::vector<AdaptiveFamilyEvidence> families = {};
  bool operator==(const AdaptivePromotionEvidence&) const = default;
};

struct AdaptiveLevelState {
  int level = 3;
  int evidence = 0;
  AdaptivePromotionEvidence promotion = {};
  bool operator==(const AdaptiveLevelState&) const = default;
};

void ValidateAdaptiveLevelState(const AdaptiveLevelState& state);

[[nodiscard]]
AdaptiveLevelState ApplyAdaptiveEvidence(AdaptiveLevelState state, int question_level, bool correct,
                                         std::size_t attempts, std::string_view family_id = {},
                                         bool application = false);

// Shared by persisted progress and v2 session snapshots. Empty objects are legacy
// state; absent family provenance is never reconstructed from current content.
[[nodiscard]] boost::json::object AdaptivePromotionToJson(const AdaptiveLevelState& state);
[[nodiscard]] AdaptivePromotionEvidence ParseAdaptivePromotion(const boost::json::object& object, int level);
