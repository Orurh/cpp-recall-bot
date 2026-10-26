#pragma once

#include "learning/progress_summary.h"
#include "learning/review_state.h"
#include "learning/training_plan.h"
#include "quiz/question_bank.h"

#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>

enum class TrainingRecommendationReason : std::uint8_t {
  kDueReview,
  kRecovery,
  kMissingEvidence,
  kKnowledgeGap,
  kNewSkill,
  kGeneralPractice,
};

struct TrainingRecommendation {
  TrainingRecommendationReason reason = TrainingRecommendationReason::kGeneralPractice;
  std::string question_id;
  std::string skill_id;
  std::string topic;
  int level = 1;
  std::string title;
  std::string description;
};

struct LearningRouteStep {
  std::string title;
  bool completed = false;
};

struct LearningRoute {
  std::string id;
  std::string title;
  std::vector<std::string> methods;
  std::string next_action;
  std::string description;
  std::string status = "unavailable";
  std::vector<LearningRouteStep> steps;
  TrainingPlan plan;
};

[[nodiscard]] LearningRoute PlanContainerLearningRoute(
    const QuestionBank& questions, const UserProgressSummary& summary,
    const std::unordered_map<std::string, QuestionReviewState>& review_states,
    const std::unordered_set<std::string>& clean_question_families);

[[nodiscard]] LearningRoute PlanLearningRoute(std::string_view route_id, const QuestionBank& questions,
                                              const UserProgressSummary& summary,
                                              const std::unordered_map<std::string, QuestionReviewState>& review_states,
                                              const std::unordered_set<std::string>& clean_question_families);

[[nodiscard]]
constexpr std::string_view TrainingRecommendationReasonName(TrainingRecommendationReason reason) {
  switch (reason) {
    case TrainingRecommendationReason::kDueReview:
      return "due_review";
    case TrainingRecommendationReason::kRecovery:
      return "recovery";
    case TrainingRecommendationReason::kMissingEvidence:
      return "missing_evidence";
    case TrainingRecommendationReason::kKnowledgeGap:
      return "knowledge_gap";
    case TrainingRecommendationReason::kNewSkill:
      return "new_skill";
    case TrainingRecommendationReason::kGeneralPractice:
      return "general_practice";
  }
  return "general_practice";
}

[[nodiscard]]
TrainingRecommendation RecommendTraining(const QuestionBank& questions, const UserProgressSummary& summary,
                                         const std::unordered_map<std::string, QuestionReviewState>& review_states,
                                         const std::unordered_set<std::string>& clean_question_families);

[[nodiscard]] TrainingPlan PlanRecommendedTraining(
    const QuestionBank& questions, const UserProgressSummary& summary,
    const std::unordered_map<std::string, QuestionReviewState>& review_states,
    const std::unordered_set<std::string>& clean_question_families, const QuestionSelection& selection = {});
