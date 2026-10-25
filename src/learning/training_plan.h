#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

enum class TrainingQuestionPurpose : std::uint8_t {
  kNewMaterial,
  kDueReview,
  kFamiliarVariant,
  kEarlyReview,
};

constexpr std::string_view TrainingQuestionPurposeName(TrainingQuestionPurpose purpose) {
  switch (purpose) {
    case TrainingQuestionPurpose::kNewMaterial:
      return "new";
    case TrainingQuestionPurpose::kDueReview:
      return "review";
    case TrainingQuestionPurpose::kFamiliarVariant:
      return "variant";
    case TrainingQuestionPurpose::kEarlyReview:
      return "early_review";
  }
  return "early_review";
}

struct PlannedTrainingQuestion {
  std::string question_id;
  TrainingQuestionPurpose purpose = TrainingQuestionPurpose::kNewMaterial;
};

struct TrainingPlan {
  // Empty for manual practice and snapshots created before balanced planning.
  std::vector<PlannedTrainingQuestion> questions = {};
  std::size_t new_question_target = 0;
};
