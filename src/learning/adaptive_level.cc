#include "adaptive_level.h"

#include <algorithm>
#include <cstddef>
#include <stdexcept>
#include <unordered_set>

#include <boost/json.hpp>

namespace {

constexpr int kMinLevel = 1;
constexpr int kMaxLevel = 10;
constexpr int kMaximumLegacyEvidence = 3;
constexpr int kDemotionThreshold = -4;
constexpr std::size_t kMaximumTrackedAttempts = 1'000;
constexpr std::size_t kMaximumTrackedFamilies = 1'000;

bool ValidFamilyId(std::string_view id) {
  return !id.empty() && id.size() <= 256 && std::ranges::all_of(id, [](unsigned char ch) {
    return (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9') || ch == '.' || ch == '_' ||
           ch == '-' || ch == ':';
  });
}

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

  if (state.evidence <= kDemotionThreshold || state.evidence > kMaximumLegacyEvidence) {
    throw std::invalid_argument("adaptive evidence must be between -3 and 3");
  }

  if ((state.promotion.level != 0 && state.promotion.level != state.level) ||
      (!state.promotion.families.empty() && state.promotion.level != state.level) ||
      state.promotion.families.size() > kMaximumTrackedFamilies) {
    throw std::invalid_argument("adaptive promotion evidence has an invalid level or size");
  }
  std::unordered_set<std::string_view> families;
  for (const auto& family : state.promotion.families) {
    if (!ValidFamilyId(family.id) || !families.insert(family.id).second)
      throw std::invalid_argument("adaptive promotion evidence has an invalid or duplicate family");
  }
}

AdaptiveLevelState ApplyAdaptiveEvidence(AdaptiveLevelState state, int question_level, bool correct,
                                         std::size_t attempts, std::string_view family_id, bool application) {
  ValidateAdaptiveLevelState(state);

  if (question_level < kMinLevel || question_level > kMaxLevel) {
    throw std::invalid_argument("question level must be between 1 and 10");
  }

  if (attempts < 1 || attempts > kMaximumTrackedAttempts) {
    throw std::invalid_argument("attempt count is outside its allowed range");
  }

  if (!family_id.empty() && !ValidFamilyId(family_id))
    throw std::invalid_argument("adaptive promotion family id is invalid");

  const auto delta = EvidenceDelta(state, question_level, correct, attempts);
  if (question_level < state.level)
    return state;
  // Legacy positive counters remain readable but cannot substitute for family
  // provenance. Clean answers still offset difficulty evidence from repairs.
  state.evidence = std::min(0, std::min(0, state.evidence) + delta);

  if (question_level == state.level && attempts == 1 && !family_id.empty()) {
    state.promotion.level = state.level;
    const auto family = std::ranges::find(state.promotion.families, family_id, &AdaptiveFamilyEvidence::id);
    if (family == state.promotion.families.end()) {
      if (state.promotion.families.size() == kMaximumTrackedFamilies)
        throw std::invalid_argument("adaptive promotion evidence exceeds its family limit");
      state.promotion.families.push_back({std::string(family_id), application});
    } else {
      family->application = family->application || application;
    }
  }

  if (question_level == state.level && attempts == 1 && !family_id.empty() && state.promotion.families.size() >= 3 &&
      std::ranges::any_of(state.promotion.families, &AdaptiveFamilyEvidence::application)) {
    state.level = std::min(state.level + 1, kMaxLevel);
    state.evidence = 0;
    state.promotion = {};
  } else if (state.evidence <= kDemotionThreshold) {
    state.level = std::max(state.level - 1, kMinLevel);
    state.evidence = 0;
    state.promotion = {};
  }

  return state;
}

boost::json::object AdaptivePromotionToJson(const AdaptiveLevelState& state) {
  ValidateAdaptiveLevelState(state);
  if (state.promotion.level == 0)
    return {};
  boost::json::array families;
  for (const auto& family : state.promotion.families)
    families.emplace_back(boost::json::object{{"id", family.id}, {"application", family.application}});
  return {{"level", state.promotion.level}, {"families", std::move(families)}};
}

AdaptivePromotionEvidence ParseAdaptivePromotion(const boost::json::object& object, int level) {
  if (object.empty())
    return {};
  const auto* recorded_level = object.if_contains("level");
  const auto* families = object.if_contains("families");
  if (!recorded_level || !recorded_level->is_int64() || recorded_level->as_int64() != level || !families ||
      !families->is_array() || families->as_array().size() > kMaximumTrackedFamilies)
    throw std::invalid_argument("stored adaptive promotion evidence has an invalid level or shape");
  AdaptiveLevelState state{.level = level};
  state.promotion.level = level;
  for (const auto& value : families->as_array()) {
    if (!value.is_object())
      throw std::invalid_argument("stored adaptive promotion family must be an object");
    const auto* id = value.as_object().if_contains("id");
    const auto* application = value.as_object().if_contains("application");
    if (!id || !id->is_string() || !application || !application->is_bool())
      throw std::invalid_argument("stored adaptive promotion family has an invalid shape");
    state.promotion.families.push_back({std::string(id->as_string()), application->as_bool()});
  }
  ValidateAdaptiveLevelState(state);
  return state.promotion;
}
