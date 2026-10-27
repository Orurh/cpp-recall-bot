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
};

[[nodiscard]] std::span<const TheoryBlock> TheoryCatalog();

[[nodiscard]] const TheoryBlock* FindTheoryForSkill(std::string_view skill_id);

void ValidateTheoryCatalog();
