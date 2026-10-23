#pragma once

#include <span>
#include <string_view>
#include <vector>

struct SkillDefinition {
  std::string_view id;
  std::string_view topic;
  std::string_view title;
  std::vector<std::string_view> prerequisites;
};

std::span<const SkillDefinition> SkillCatalog();
std::span<const std::string_view> MasteryPilotSkillIds();
const SkillDefinition* FindSkill(std::string_view id);
void ValidateSkillCatalog();
