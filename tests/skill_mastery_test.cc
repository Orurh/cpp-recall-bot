#include "learning/skill_mastery.h"

#include <gtest/gtest.h>

namespace {

SkillEvidenceSummary DemonstratedEvidence() {
  return {
      .successful_family_count = 2,
      .clean_family_count = 2,
      .application_family_count = 1,
      .transfer_family_count = 1,
  };
}

}  // namespace

TEST(SkillMasteryTest, ExhaustedQuestionFamiliesCanStillRecover) {
  const auto transition =
      ApplySkillMastery(SkillMasteryStatus::kNeedsRecovery, DemonstratedEvidence(),
                        SkillMasteryEvent{.clean_application = true, .recovery_evidence_after_start = true});

  EXPECT_EQ(transition.current, SkillMasteryStatus::kDemonstrated);
}

TEST(SkillMasteryTest, RetainedIsReachableWithDelayedApplication) {
  const auto transition =
      ApplySkillMastery(SkillMasteryStatus::kDemonstrated, DemonstratedEvidence(),
                        SkillMasteryEvent{.clean_application = true, .delayed_retention_evidence = true});

  EXPECT_EQ(transition.current, SkillMasteryStatus::kRetained);
}

TEST(SkillMasteryTest, RecoveryStartsANewRetentionCycle) {
  const auto evidence = DemonstratedEvidence();
  const auto failure =
      ApplySkillMastery(SkillMasteryStatus::kRetained, evidence, SkillMasteryEvent{.due_failure = true});
  ASSERT_EQ(failure.current, SkillMasteryStatus::kNeedsRecovery);

  const auto recovery = ApplySkillMastery(
      failure.current, evidence, SkillMasteryEvent{.clean_application = true, .recovery_evidence_after_start = true});
  ASSERT_EQ(recovery.current, SkillMasteryStatus::kDemonstrated);

  const auto immediate = ApplySkillMastery(recovery.current, evidence, SkillMasteryEvent{.clean_application = true});
  EXPECT_EQ(immediate.current, SkillMasteryStatus::kDemonstrated);

  const auto retained = ApplySkillMastery(
      immediate.current, evidence, SkillMasteryEvent{.clean_application = true, .delayed_retention_evidence = true});
  EXPECT_EQ(retained.current, SkillMasteryStatus::kRetained);
}
