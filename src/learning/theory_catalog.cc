#include "theory_catalog.h"

#include "learning/module_catalog.h"
#include "learning/skill_catalog.h"

#include <algorithm>
#include <array>
#include <stdexcept>
#include <string>
#include <unordered_set>

namespace {

#include "learning/theory_catalog_data.inc"

}  // namespace

std::span<const TheoryBlock> TheoryCatalog() {
  return kTheoryBlocks;
}

const TheoryBlock* FindTheoryForSkill(std::string_view skill_id) {
  const auto it = std::ranges::find_if(
      kTheoryBlocks, [skill_id](const auto& block) { return block.skill_id == skill_id && block.primary; });
  return it == kTheoryBlocks.end() ? nullptr : &*it;
}

const TheoryBlock* FindTheoryForQuestion(std::string_view skill_id, std::string_view question_family_id) {
  const auto binding = std::ranges::find_if(kTheoryBindings, [&](const auto& candidate) {
    return candidate.skill_id == skill_id && candidate.question_family_id == question_family_id;
  });
  if (binding != std::end(kTheoryBindings)) {
    const auto target = std::ranges::find(kTheoryBlocks, binding->theory_id, &TheoryBlock::id);
    if (target != kTheoryBlocks.end() && target->skill_id == skill_id)
      return &*target;
  }
  return FindTheoryForSkill(skill_id);
}

void ValidateTheoryBindings(std::span<const TheoryBlock> blocks, std::span<const TheoryBinding> bindings) {
  std::unordered_set<std::string> families;
  for (const auto& binding : bindings) {
    if (binding.skill_id.empty() || binding.question_family_id.empty() || binding.theory_id.empty())
      throw std::logic_error("theory binding fields must not be empty");
    if (!families.insert(std::string(binding.skill_id) + ":" + std::string(binding.question_family_id)).second)
      throw std::logic_error("duplicate theory family binding: " + std::string(binding.question_family_id));
    const auto target = std::ranges::find(blocks, binding.theory_id, &TheoryBlock::id);
    if (target == blocks.end() || std::ranges::count(blocks, binding.theory_id, &TheoryBlock::id) != 1)
      throw std::logic_error("theory binding must have one existing target: " + std::string(binding.theory_id));
    if (target->skill_id != binding.skill_id)
      throw std::logic_error("theory binding crosses skills: " + std::string(binding.theory_id));
  }
}

void ValidateTheoryCatalog() {
  std::unordered_set<std::string_view> ids;
  std::unordered_set<std::string_view> skills;
  for (const auto& block : kTheoryBlocks) {
    if (block.id.empty() || block.skill_id.empty() || block.title.empty() || block.principle.empty() ||
        block.mechanism.empty() || block.example.empty() || block.common_mistake.empty() || block.sources.empty()) {
      throw std::logic_error("theory block fields must not be empty");
    }
    if (FindSkill(block.skill_id) == nullptr) {
      throw std::logic_error("theory block references unknown skill: " + std::string(block.skill_id));
    }
    if (!ids.insert(block.id).second) {
      throw std::logic_error("duplicate theory block id: " + std::string(block.id));
    }
    if (block.primary && !skills.insert(block.skill_id).second) {
      throw std::logic_error("a skill has more than one primary theory block: " + std::string(block.skill_id));
    }
    for (const auto source : block.sources) {
      const bool legacy_cpp =
          block.source_kind == LearningSourceKind::kCppStandard && source.starts_with("https://eel.is/c++draft/");
      if (!legacy_cpp && !IsLearningSource(source, block.source_kind)) {
        throw std::logic_error("theory must use a primary source for its subject: " + std::string(block.id));
      }
    }
  }

  ValidateTheoryBindings(kTheoryBlocks, kTheoryBindings);

  const auto* pilot = FindLearningModule("lifetime_raii");
  if (pilot == nullptr) {
    throw std::logic_error("the lifetime and RAII pilot module is missing");
  }
  for (const auto skill_id : pilot->skill_ids) {
    if (FindTheoryForSkill(skill_id) == nullptr) {
      throw std::logic_error("the pilot module has no theory block for skill: " + std::string(skill_id));
    }
  }
}
