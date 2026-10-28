#pragma once

#include "quiz/question_bank.h"

#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

enum class LearningRouteStageKind : std::uint8_t { kPractice, kIndependent };

struct LearningRouteStageDefinition {
  std::string_view id;
  std::string_view title;
  LearningRouteStageKind kind;
  std::vector<std::string_view> question_ids;
};

struct LearningRouteDefinition {
  std::string_view id;
  std::string_view title;
  std::vector<std::string_view> topics;
  std::vector<std::string_view> methods;
  std::string_view retention_skill_id;
  std::vector<LearningRouteStageDefinition> stages;
  bool requires_retention = true;
};

[[nodiscard]] std::span<const LearningRouteDefinition> LearningRouteCatalog();
[[nodiscard]] const LearningRouteDefinition* FindLearningRoute(std::string_view id);
void ValidateLearningRouteCatalog(const QuestionBank& bank);
