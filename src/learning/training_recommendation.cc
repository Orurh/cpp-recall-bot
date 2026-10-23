#include "learning/training_recommendation.h"

#include "learning/skill_catalog.h"
#include "quiz/question.h"

#include <algorithm>
#include <cstdlib>
#include <limits>
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
  const Question* best = nullptr;
  auto best_score = std::numeric_limits<int>::min();

  for (const auto& id : bank.QuestionIds()) {
    const auto& question = bank.Get(id);
    if (question.skill_id != skill_id || !IsSafeNewQuestion(question, summary)) {
      continue;
    }

    const bool family_is_new = !clean_question_families.contains(question.question_family_id);
    const bool question_is_unseen = !review_states.contains(question.id);
    const bool is_application = question.evidence_type == QuestionEvidenceType::kApplication;
    const int distance = std::abs(question.level - TopicLevel(summary, question.topic));
    const int score = (family_is_new ? 1'000 : 0) + (question_is_unseen ? 200 : 0) +
                      (prefer_application && is_application ? 100 : 0) + (question.transfer ? 40 : 0) - distance * 10;

    if (best == nullptr || score > best_score || (score == best_score && question.id < best->id)) {
      best = &question;
      best_score = score;
    }
  }
  return best;
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
