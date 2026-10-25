#include "learning/training_recommendation.h"

#include "learning/skill_catalog.h"
#include "quiz/question.h"

#include <algorithm>
#include <cstdlib>
#include <limits>
#include <random>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace {

using SkillStates = std::unordered_map<std::string, const SkillProgressSummary*>;

const SkillProgressSummary* FindProgress(const SkillStates& states, std::string_view skill_id) {
  const auto it = states.find(std::string(skill_id));
  return it == states.end() ? nullptr : it->second;
}

SkillMasteryStatus StatusOf(const SkillStates& states, std::string_view skill_id) {
  const auto* progress = FindProgress(states, skill_id);
  return progress == nullptr ? SkillMasteryStatus::kUnassessed : progress->mastery_status;
}

int TopicLevel(const UserProgressSummary& summary, std::string_view topic) {
  const auto it = std::ranges::find(summary.topics, topic, &TopicProgressSummary::topic);
  return it == summary.topics.end() ? summary.adaptive_level : it->adaptive_level;
}

bool IsSafeNewQuestion(const Question& question, const UserProgressSummary& summary) {
  return question.level <= summary.unlocked_level && question.level <= TopicLevel(summary, question.topic);
}

bool PrerequisitesReady(const SkillDefinition& skill, const SkillStates& states) {
  return std::ranges::all_of(skill.prerequisites, [&states](std::string_view prerequisite) {
    const auto status = StatusOf(states, prerequisite);
    if (IsMasteryPilotSkill(prerequisite)) {
      return status == SkillMasteryStatus::kDemonstrated || status == SkillMasteryStatus::kRetained;
    }
    return status != SkillMasteryStatus::kUnassessed && status != SkillMasteryStatus::kNeedsRecovery;
  });
}

std::string SkillTitle(std::string_view skill_id) {
  const auto* skill = FindSkill(skill_id);
  return skill == nullptr ? std::string(skill_id) : std::string(skill->title);
}

TrainingRecommendation MakeRecommendation(TrainingRecommendationReason reason, const Question& question) {
  const auto skill_title = SkillTitle(question.skill_id);
  TrainingRecommendation result{
      .reason = reason,
      .question_id = question.id,
      .skill_id = question.skill_id,
      .topic = question.topic,
      .level = question.level,
      .title = {},
      .description = {},
  };

  switch (reason) {
    case TrainingRecommendationReason::kDueReview:
      result.title = "Пора повторить: " + skill_title;
      result.description = "Этот материал уже созрел для отложенной проверки.";
      break;
    case TrainingRecommendationReason::kRecovery:
      result.title = "Восстановим: " + skill_title;
      result.description = "После ошибки на повторении проверим навык на новом примере.";
      break;
    case TrainingRecommendationReason::kMissingEvidence:
      result.title = "Подтвердим: " + skill_title;
      result.description = "Для подтверждения не хватает чистого ответа из независимого семейства.";
      break;
    case TrainingRecommendationReason::kKnowledgeGap:
      result.title = "Заполним пробел: " + skill_title;
      result.description = "В знакомой теме остался ещё не проверенный базовый навык.";
      break;
    case TrainingRecommendationReason::kNewSkill:
      result.title = "Следующий навык: " + skill_title;
      result.description = "Предыдущие необходимые знания уже пройдены; можно двигаться дальше.";
      break;
    case TrainingRecommendationReason::kGeneralPractice:
      result.title = "Закрепим текущий уровень";
      result.description = "Просроченных повторений нет — возьмём подходящий новый вопрос.";
      break;
  }
  return result;
}

const Question* BestQuestionForSkill(const QuestionBank& bank, std::string_view skill_id,
                                     const UserProgressSummary& summary,
                                     const std::unordered_map<std::string, QuestionReviewState>& review_states,
                                     const std::unordered_set<std::string>& clean_question_families,
                                     bool prefer_application) {
  const auto find_best = [&](bool require_application) {
    const Question* best = nullptr;
    auto best_score = std::numeric_limits<int>::min();

    for (const auto& id : bank.QuestionIds()) {
      const auto& question = bank.Get(id);
      const bool is_application = question.evidence_type == QuestionEvidenceType::kApplication;
      if (question.skill_id != skill_id || !IsSafeNewQuestion(question, summary) ||
          (require_application && !is_application)) {
        continue;
      }

      const bool family_is_new = !clean_question_families.contains(question.question_family_id);
      const bool question_is_unseen = !review_states.contains(question.id);
      const int distance = std::abs(question.level - TopicLevel(summary, question.topic));
      const int score =
          (family_is_new ? 1'000 : 0) + (question_is_unseen ? 200 : 0) + (question.transfer ? 40 : 0) - distance * 10;

      if (best == nullptr || score > best_score || (score == best_score && question.id < best->id)) {
        best = &question;
        best_score = score;
      }
    }
    return best;
  };

  if (prefer_application) {
    if (const auto* application = find_best(true)) {
      return application;
    }
  }
  return find_best(false);
}

}  // namespace

TrainingRecommendation RecommendTraining(const QuestionBank& questions, const UserProgressSummary& summary,
                                         const std::unordered_map<std::string, QuestionReviewState>& review_states,
                                         const std::unordered_set<std::string>& clean_question_families) {
  if (summary.adaptive_level < 1 || summary.adaptive_level > 10 || summary.unlocked_level < summary.adaptive_level ||
      summary.unlocked_level > 10) {
    throw std::invalid_argument("invalid progress levels for training recommendation");
  }

  SkillStates states;
  states.reserve(summary.skills.size());
  for (const auto& skill : summary.skills) {
    states.insert_or_assign(skill.skill_id, &skill);
  }

  const Question* due = nullptr;
  for (const auto& id : questions.QuestionIds()) {
    const auto state = review_states.find(id);
    if (state == review_states.end() || !state->second.due) {
      continue;
    }
    const auto& question = questions.Get(id);
    if (question.level > summary.unlocked_level) {
      continue;
    }
    if (due == nullptr) {
      due = &question;
      continue;
    }
    const auto& best_state = review_states.at(due->id);
    const bool more_urgent =
        state->second.stage < best_state.stage ||
        (state->second.stage == best_state.stage && state->second.failed_count > best_state.failed_count);
    if (more_urgent || (state->second.stage == best_state.stage &&
                        state->second.failed_count == best_state.failed_count && question.id < due->id)) {
      due = &question;
    }
  }
  if (due != nullptr) {
    return MakeRecommendation(TrainingRecommendationReason::kDueReview, *due);
  }

  for (const auto& skill : summary.skills) {
    if (skill.mastery_status != SkillMasteryStatus::kNeedsRecovery) {
      continue;
    }
    if (const auto* question =
            BestQuestionForSkill(questions, skill.skill_id, summary, review_states, clean_question_families, true)) {
      return MakeRecommendation(TrainingRecommendationReason::kRecovery, *question);
    }
  }

  std::vector<const SkillProgressSummary*> evidence_targets;
  for (const auto& skill : summary.skills) {
    if (!IsMasteryPilotSkill(skill.skill_id) || (skill.mastery_status != SkillMasteryStatus::kApplying &&
                                                 skill.mastery_status != SkillMasteryStatus::kEncountered)) {
      continue;
    }
    evidence_targets.push_back(&skill);
  }
  std::ranges::sort(evidence_targets, [](const SkillProgressSummary* lhs, const SkillProgressSummary* rhs) {
    if (lhs->mastery_status != rhs->mastery_status) {
      return lhs->mastery_status == SkillMasteryStatus::kApplying;
    }
    if (lhs->clean_family_count != rhs->clean_family_count) {
      return lhs->clean_family_count > rhs->clean_family_count;
    }
    return lhs->skill_id < rhs->skill_id;
  });
  for (const auto* evidence_target : evidence_targets) {
    if (const auto* question =
            BestQuestionForSkill(questions, evidence_target->skill_id, summary, review_states, clean_question_families,
                                 evidence_target->application_family_count == 0)) {
      return MakeRecommendation(TrainingRecommendationReason::kMissingEvidence, *question);
    }
  }

  std::unordered_set<std::string_view> practiced_topics;
  practiced_topics.reserve(summary.topics.size());
  for (const auto& topic : summary.topics) {
    practiced_topics.insert(topic.topic);
  }

  const auto find_new_skill = [&](bool require_practiced_topic) -> const Question* {
    for (const auto skill_id : MasteryPilotSkillIds()) {
      const auto* definition = FindSkill(skill_id);
      if (definition == nullptr || StatusOf(states, skill_id) != SkillMasteryStatus::kUnassessed ||
          !PrerequisitesReady(*definition, states) ||
          (require_practiced_topic && !practiced_topics.contains(definition->topic))) {
        continue;
      }
      if (const auto* question =
              BestQuestionForSkill(questions, skill_id, summary, review_states, clean_question_families, false)) {
        return question;
      }
    }
    return nullptr;
  };

  if (const auto* gap = find_new_skill(true)) {
    return MakeRecommendation(TrainingRecommendationReason::kKnowledgeGap, *gap);
  }
  if (const auto* next = find_new_skill(false)) {
    return MakeRecommendation(TrainingRecommendationReason::kNewSkill, *next);
  }

  const Question* fallback = nullptr;
  int best_distance = std::numeric_limits<int>::max();
  for (const auto& id : questions.QuestionIds()) {
    const auto& question = questions.Get(id);
    if (!IsSafeNewQuestion(question, summary)) {
      continue;
    }
    const int distance = std::abs(question.level - TopicLevel(summary, question.topic));
    const bool unseen = !review_states.contains(question.id);
    const bool best_unseen = fallback != nullptr && !review_states.contains(fallback->id);
    if (fallback == nullptr || (unseen && !best_unseen) || (unseen == best_unseen && distance < best_distance) ||
        (unseen == best_unseen && distance == best_distance && question.id < fallback->id)) {
      fallback = &question;
      best_distance = distance;
    }
  }
  if (fallback == nullptr) {
    throw std::runtime_error("no safe question is available for recommended training");
  }
  return MakeRecommendation(TrainingRecommendationReason::kGeneralPractice, *fallback);
}

TrainingPlan PlanRecommendedTraining(const QuestionBank& questions, const UserProgressSummary& summary,
                                     const std::unordered_map<std::string, QuestionReviewState>& review_states,
                                     const std::unordered_set<std::string>& clean_question_families) {
  if (summary.adaptive_level < 1 || summary.adaptive_level > 10 || summary.unlocked_level < summary.adaptive_level ||
      summary.unlocked_level > 10) {
    throw std::invalid_argument("invalid progress levels for training plan");
  }
  constexpr std::size_t kGoal = 10;
  constexpr std::size_t kNewTarget = 6;
  constexpr std::size_t kReviewTarget = kGoal - kNewTarget;

  SkillStates states;
  for (const auto& skill : summary.skills) {
    states.emplace(skill.skill_id, &skill);
  }
  auto seen_families = clean_question_families;
  const auto ids = questions.QuestionIds();
  for (const auto& id : ids) {
    if (review_states.contains(id)) {
      seen_families.insert(questions.Get(id).question_family_id);
    }
  }

  struct Candidate {
    const Question* question;
    const QuestionReviewState* review;
    int distance;
  };
  std::vector<Candidate> fresh, due, variants, early;
  for (const auto& id : ids) {
    const auto& question = questions.Get(id);
    if (question.level > summary.unlocked_level) {
      continue;
    }
    const auto found = review_states.find(id);
    const auto* review = found == review_states.end() ? nullptr : &found->second;
    const Candidate candidate{&question, review, std::abs(question.level - TopicLevel(summary, question.topic))};
    if (review != nullptr) {
      (review->due ? due : early).push_back(candidate);
      continue;
    }
    const auto* definition = FindSkill(question.skill_id);
    if (!IsSafeNewQuestion(question, summary) || definition == nullptr || !PrerequisitesReady(*definition, states)) {
      continue;
    }
    (seen_families.contains(question.question_family_id) ? variants : fresh).push_back(candidate);
  }

  static thread_local std::mt19937 generator{std::random_device{}()};
  for (auto* pool : {&fresh, &due, &variants, &early}) {
    std::ranges::shuffle(*pool, generator);
    std::ranges::stable_sort(*pool, [](const Candidate& left, const Candidate& right) {
      if (left.review != nullptr && right.review != nullptr) {
        if (!left.review->due && left.review->seconds_until_due != right.review->seconds_until_due) {
          return left.review->seconds_until_due < right.review->seconds_until_due;
        }
        if (left.review->stage != right.review->stage) {
          return left.review->stage < right.review->stage;
        }
        if (left.review->failed_count != right.review->failed_count) {
          return left.review->failed_count > right.review->failed_count;
        }
        if (left.review->seconds_until_due != right.review->seconds_until_due) {
          return left.review->seconds_until_due < right.review->seconds_until_due;
        }
      }
      return left.distance < right.distance;
    });
  }

  TrainingPlan plan{.new_question_target = kNewTarget};
  std::unordered_set<std::string> selected_families;
  std::unordered_map<std::string, std::size_t> topic_counts;
  std::unordered_map<std::string, std::size_t> review_topic_counts;
  const auto take = [&](const std::vector<Candidate>& pool, std::size_t limit, TrainingQuestionPurpose purpose) {
    auto& counts = purpose == TrainingQuestionPurpose::kDueReview ? review_topic_counts : topic_counts;
    for (std::size_t count = 0; count < limit && plan.questions.size() < kGoal; ++count) {
      const Candidate* best = nullptr;
      std::size_t best_topic_count = kGoal + 1;
      for (const auto& candidate : pool) {
        if (selected_families.contains(candidate.question->question_family_id)) {
          continue;
        }
        const auto topic_count = counts[candidate.question->topic];
        if (best == nullptr || topic_count < best_topic_count) {
          best = &candidate;
          best_topic_count = topic_count;
        }
      }
      if (best == nullptr) {
        break;
      }
      plan.questions.push_back({best->question->id, purpose});
      selected_families.insert(best->question->question_family_id);
      ++counts[best->question->topic];
    }
  };
  take(fresh, kNewTarget, TrainingQuestionPurpose::kNewMaterial);
  take(due, kReviewTarget, TrainingQuestionPurpose::kDueReview);
  // An unavailable category never blocks the other one. Due dates of unselected
  // questions stay untouched; fixes belong to LearningQueue, not this budget.
  take(fresh, kGoal, TrainingQuestionPurpose::kNewMaterial);
  take(due, kGoal, TrainingQuestionPurpose::kDueReview);
  take(variants, kGoal, TrainingQuestionPurpose::kFamiliarVariant);
  take(early, kGoal, TrainingQuestionPurpose::kEarlyReview);
  if (plan.questions.empty()) {
    throw std::runtime_error("no safe question is available for recommended training");
  }

  // Spread planned reviews through the primary queue instead of exhausting the
  // whole backlog before showing anything new. Keep order inside each category.
  auto selected = std::move(plan.questions);
  plan.questions.clear();
  std::vector<PlannedTrainingQuestion> learning, reviewing;
  for (auto& entry : selected) {
    (entry.purpose == TrainingQuestionPurpose::kDueReview ? reviewing : learning).push_back(std::move(entry));
  }
  std::size_t learning_index = 0, review_index = 0;
  const auto total = learning.size() + reviewing.size();
  for (std::size_t index = 0; index < total; ++index) {
    const bool review_slot = (index + 1) * reviewing.size() / total > index * reviewing.size() / total;
    plan.questions.push_back(review_slot ? std::move(reviewing[review_index++])
                                         : std::move(learning[learning_index++]));
  }
  return plan;
}
