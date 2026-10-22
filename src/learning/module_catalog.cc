#include "learning/module_catalog.h"

#include "learning/skill_catalog.h"

#include <algorithm>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <unordered_set>

namespace {

const std::vector<LearningModuleDefinition> kModules{
    {
        .id = "lifetime_raii",
        .title = "Lifetime и RAII",
        .description = "Время хранения и жизни объектов, автоматическое освобождение ресурсов и модели владения.",
        .skill_ids =
            {
                "core.storage_duration",
                "raii.fundamentals",
                "lifetime.objects",
                "lifetime.smart_pointers",
            },
        .minimum_questions_per_skill = 6,
        .minimum_applied_questions_per_skill = 3,
    },
};

bool IsApplied(QuestionKind kind) {
  return kind == QuestionKind::kCodeReading || kind == QuestionKind::kDebugging ||
         kind == QuestionKind::kImplementation;
}

}  // namespace

std::span<const LearningModuleDefinition> LearningModuleCatalog() {
  return kModules;
}

const LearningModuleDefinition* FindLearningModule(std::string_view id) {
  const auto it = std::ranges::find(kModules, id, &LearningModuleDefinition::id);
  return it == kModules.end() ? nullptr : &*it;
}

void ValidateLearningModuleCatalog() {
  ValidateSkillCatalog();

  std::unordered_set<std::string_view> module_ids;
  for (const auto& module : kModules) {
    if (module.id.empty() || module.title.empty() || module.description.empty()) {
      throw std::logic_error("learning module id, title and description must not be empty");
    }
    if (!module_ids.insert(module.id).second) {
      throw std::logic_error("duplicate learning module id: " + std::string(module.id));
    }
    if (module.skill_ids.empty()) {
      throw std::logic_error("learning module must contain at least one skill: " + std::string(module.id));
    }
    if (module.minimum_questions_per_skill == 0 || module.minimum_applied_questions_per_skill == 0 ||
        module.minimum_applied_questions_per_skill > module.minimum_questions_per_skill) {
      throw std::logic_error("learning module has invalid coverage requirements: " + std::string(module.id));
    }

    std::unordered_set<std::string_view> module_skills;
    for (std::size_t index = 0; index < module.skill_ids.size(); ++index) {
      const auto skill_id = module.skill_ids[index];
      const auto* skill = FindSkill(skill_id);
      if (skill == nullptr) {
        throw std::logic_error("learning module references unknown skill: " + std::string(skill_id));
      }
      if (!module_skills.insert(skill_id).second) {
        throw std::logic_error("learning module contains duplicate skill: " + std::string(skill_id));
      }

      for (const auto prerequisite : skill->prerequisites) {
        const auto prerequisite_position = std::ranges::find(module.skill_ids, prerequisite);
        if (prerequisite_position != module.skill_ids.end() &&
            prerequisite_position >= module.skill_ids.begin() + static_cast<std::ptrdiff_t>(index)) {
          throw std::logic_error("learning module places a skill before its prerequisite: " + std::string(skill_id));
        }
      }
    }
  }
}

void ValidateLearningModuleCoverage(std::span<const Question> questions) {
  ValidateLearningModuleCatalog();

  for (const auto& module : kModules) {
    for (const auto skill_id : module.skill_ids) {
      std::size_t question_count = 0;
      std::size_t applied_count = 0;
      for (const auto& question : questions) {
        if (question.skill_id != skill_id) {
          continue;
        }
        ++question_count;
        if (IsApplied(question.kind)) {
          ++applied_count;
        }
      }

      if (question_count < module.minimum_questions_per_skill) {
        throw std::invalid_argument("learning module '" + std::string(module.id) + "' skill '" + std::string(skill_id) +
                                    "' has only " + std::to_string(question_count) + " questions; required " +
                                    std::to_string(module.minimum_questions_per_skill));
      }
      if (applied_count < module.minimum_applied_questions_per_skill) {
        throw std::invalid_argument("learning module '" + std::string(module.id) + "' skill '" + std::string(skill_id) +
                                    "' has only " + std::to_string(applied_count) + " applied questions; required " +
                                    std::to_string(module.minimum_applied_questions_per_skill));
      }
    }
  }
}
