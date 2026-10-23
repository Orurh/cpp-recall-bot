#pragma once

#include <cstdint>
#include <string_view>

enum class SkillMasteryStatus : std::uint8_t {
  kUnassessed,
  kEncountered,
  kApplying,
  kDemonstrated,
  kRetained,
  kNeedsRecovery,
};

struct SkillEvidenceSummary {
  std::int64_t successful_family_count = 0;
  std::int64_t clean_family_count = 0;
  std::int64_t application_family_count = 0;
  std::int64_t transfer_family_count = 0;
};

struct SkillMasteryEvent {
  bool due_failure = false;
  bool clean_application = false;
  // The current evidence was recorded after needs_recovery_since.
  bool recovery_evidence_after_start = false;
  // The current evidence belongs to a later session after the retention delay.
  bool delayed_retention_evidence = false;
};

struct SkillMasteryTransition {
  SkillMasteryStatus previous = SkillMasteryStatus::kUnassessed;
  SkillMasteryStatus current = SkillMasteryStatus::kUnassessed;
};

[[nodiscard]]
constexpr std::string_view SkillMasteryStatusName(SkillMasteryStatus status) {
  switch (status) {
    case SkillMasteryStatus::kUnassessed:
      return "unassessed";
    case SkillMasteryStatus::kEncountered:
      return "encountered";
    case SkillMasteryStatus::kApplying:
      return "applying";
    case SkillMasteryStatus::kDemonstrated:
      return "demonstrated";
    case SkillMasteryStatus::kRetained:
      return "retained";
    case SkillMasteryStatus::kNeedsRecovery:
      return "needs_recovery";
  }

  return "unassessed";
}

[[nodiscard]]
SkillMasteryStatus ParseSkillMasteryStatus(std::string_view value);

[[nodiscard]]
SkillMasteryStatus AchievedSkillMastery(const SkillEvidenceSummary& evidence);

[[nodiscard]]
SkillMasteryTransition ApplySkillMastery(SkillMasteryStatus previous, const SkillEvidenceSummary& evidence,
                                         const SkillMasteryEvent& event);

[[nodiscard]]
constexpr bool IsConfirmedSkillMastery(SkillMasteryStatus status) {
  return status == SkillMasteryStatus::kDemonstrated || status == SkillMasteryStatus::kRetained ||
         status == SkillMasteryStatus::kNeedsRecovery;
}
