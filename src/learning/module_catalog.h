#pragma once

#include "quiz/question.h"

#include <cstddef>
#include <span>
#include <string_view>
#include <vector>

struct LearningModuleDefinition {
  std::string_view id;
  std::string_view title;
  std::string_view description;
  std::vector<std::string_view> skill_ids;
  std::size_t minimum_questions_per_skill = 0;
  std::size_t minimum_applied_questions_per_skill = 0;
};

std::span<const LearningModuleDefinition> LearningModuleCatalog();
const LearningModuleDefinition* FindLearningModule(std::string_view id);
void ValidateLearningModuleCatalog();
void ValidateLearningModuleCoverage(std::span<const Question> questions);
