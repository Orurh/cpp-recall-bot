#pragma once

#include "learning/content_source.h"

#include <span>
#include <string_view>
#include <vector>

struct TheoryBlock {
  std::string_view id;
  std::string_view skill_id;
  std::string_view title;
  std::string_view principle;
  std::string_view mechanism;
  std::string_view example;
  std::string_view common_mistake;
  std::vector<std::string_view> sources;
  LearningSourceKind source_kind = LearningSourceKind::kCppStandard;
  bool primary = true;
};

struct TheoryBinding {
  std::string_view skill_id;
  std::string_view question_family_id;
  std::string_view theory_id;
};

[[nodiscard]] std::span<const TheoryBlock> TheoryCatalog();

[[nodiscard]] const TheoryBlock* FindTheoryForSkill(std::string_view skill_id);

[[nodiscard]] const TheoryBlock* FindTheoryForQuestion(std::string_view skill_id, std::string_view question_family_id);

void ValidateTheoryBindings(std::span<const TheoryBlock> blocks, std::span<const TheoryBinding> bindings);

void ValidateTheoryCatalog();
