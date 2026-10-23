#include "learning/skill_mastery.h"

#include <stdexcept>
#include <string>

SkillMasteryStatus ParseSkillMasteryStatus(std::string_view value) {
  if (value == "unassessed")
    return SkillMasteryStatus::kUnassessed;
  if (value == "encountered")
    return SkillMasteryStatus::kEncountered;
  if (value == "applying")
    return SkillMasteryStatus::kApplying;
  if (value == "demonstrated")
    return SkillMasteryStatus::kDemonstrated;
  if (value == "retained")
    return SkillMasteryStatus::kRetained;
  if (value == "needs_recovery")
    return SkillMasteryStatus::kNeedsRecovery;

  throw std::invalid_argument("unknown skill mastery status: " + std::string(value));
}

SkillMasteryStatus AchievedSkillMastery(const SkillEvidenceSummary& evidence) {
  if (evidence.successful_family_count < 0 || evidence.clean_family_count < 0 ||
      evidence.application_family_count < 0 || evidence.transfer_family_count < 0 ||
      evidence.delayed_application_count < 0) {
    throw std::invalid_argument("skill evidence counts must not be negative");
  }

  if (evidence.delayed_application_count > 0) {
    return SkillMasteryStatus::kRetained;
  }
  if (evidence.clean_family_count >= 2 && evidence.application_family_count >= 1) {
    return SkillMasteryStatus::kDemonstrated;
  }
  if (evidence.application_family_count >= 1) {
    return SkillMasteryStatus::kApplying;
  }
  if (evidence.successful_family_count >= 1) {
    return SkillMasteryStatus::kEncountered;
  }
  return SkillMasteryStatus::kUnassessed;
}

SkillMasteryTransition ApplySkillMastery(SkillMasteryStatus previous, const SkillEvidenceSummary& evidence,
                                         const SkillMasteryEvent& event) {
  auto current = AchievedSkillMastery(evidence);
  const bool was_confirmed = previous == SkillMasteryStatus::kDemonstrated ||
                             previous == SkillMasteryStatus::kRetained ||
                             previous == SkillMasteryStatus::kNeedsRecovery;

  if (event.due_failure && was_confirmed) {
    current = SkillMasteryStatus::kNeedsRecovery;
  } else if (previous == SkillMasteryStatus::kNeedsRecovery &&
             !(event.clean_application && event.fresh_recovery_evidence)) {
    current = SkillMasteryStatus::kNeedsRecovery;
  }

  return {
      .previous = previous,
      .current = current,
  };
}
