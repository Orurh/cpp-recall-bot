#include "learning/training_recommendation.h"

#include <algorithm>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace {

using Reviews = std::unordered_map<std::string, QuestionReviewState>;

void Check(bool condition, const char* message) {
  if (!condition) {
    throw std::runtime_error(message);
  }
}

Question Card(std::string id, bool application = false) {
  Question question{};
  question.id = std::move(id);
  question.topic = "language_core";
  question.skill_id = "core.scope";
  question.question_family_id = "family." + question.id;
  question.evidence_type = application ? QuestionEvidenceType::kApplication : QuestionEvidenceType::kRecognition;
  question.level = 2;
  question.task = SingleChoiceTask{.options = {{"a", "Correct"}, {"b", "Wrong"}}, .correct_option_id = "a"};
  return question;
}

UserProgressSummary Summary(SkillMasteryStatus status = SkillMasteryStatus::kNeedsRecovery) {
  return {.adaptive_level = 3,
          .unlocked_level = 4,
          .topics = {{.topic = "language_core", .adaptive_level = 3}, {.topic = "containers", .adaptive_level = 3}},
          .skills = {{.skill_id = "core.scope", .mastery_status = status}}};
}

Reviews CompetingReviews() {
  return {{"urgent", {.due = true, .stage = 0, .seconds_until_due = -100000, .failed_count = 99}},
          {"application", {.due = true, .stage = 5, .seconds_until_due = -1}}};
}

const PlannedTrainingQuestion& FirstReview(const TrainingPlan& plan) {
  const auto it =
      std::ranges::find(plan.questions, TrainingQuestionPurpose::kDueReview, &PlannedTrainingQuestion::purpose);
  Check(it != plan.questions.end(), "expected a due review in the plan");
  return *it;
}

bool Contains(const TrainingPlan& plan, const std::string& id) {
  return std::ranges::find(plan.questions, id, &PlannedTrainingQuestion::question_id) != plan.questions.end();
}

void CheckRecoveryApplicationFirst() {
  const QuestionBank bank({Card("urgent"), Card("application", true)});
  const auto reviews = CompetingReviews();
  const auto recommendation = RecommendTraining(bank, Summary(), reviews, {});
  Check(
      recommendation.question_id == "application" && recommendation.reason == TrainingRecommendationReason::kDueReview,
      "a due application of a recovery skill lost to ordinary review urgency");
  Check(FirstReview(PlanRecommendedTraining(bank, Summary(), reviews, {})).question_id == "application",
        "mixed planner did not prioritize a due recovery application");
}

void CheckFallbacks() {
  for (const auto status :
       {SkillMasteryStatus::kUnassessed, SkillMasteryStatus::kEncountered, SkillMasteryStatus::kApplying,
        SkillMasteryStatus::kDemonstrated, SkillMasteryStatus::kRetained}) {
    const QuestionBank bank({Card("urgent"), Card("application", true)});
    const auto reviews = CompetingReviews();
    Check(RecommendTraining(bank, Summary(status), reviews, {}).question_id == "urgent" &&
              FirstReview(PlanRecommendedTraining(bank, Summary(status), reviews, {})).question_id == "urgent",
          "application alone changed ordinary due urgency without a recovery skill");
  }
  const QuestionBank recognition_bank({Card("urgent"), Card("application")});
  const auto reviews = CompetingReviews();
  Check(RecommendTraining(recognition_bank, Summary(), reviews, {}).question_id == "urgent" &&
            FirstReview(PlanRecommendedTraining(recognition_bank, Summary(), reviews, {})).question_id == "urgent",
        "a recognition card gained application priority");
  auto unknown = Summary();
  unknown.skills.clear();
  const QuestionBank bank({Card("urgent"), Card("application", true)});
  Check(RecommendTraining(bank, unknown, reviews, {}).question_id == "urgent" &&
            FirstReview(PlanRecommendedTraining(bank, unknown, reviews, {})).question_id == "urgent",
        "missing skill history was treated as recovery");
  auto early = reviews;
  early.at("application").due = false;
  early.at("application").seconds_until_due = 86400;
  const auto plan = PlanRecommendedTraining(bank, Summary(), early, {});
  const auto early_entry = std::ranges::find(plan.questions, "application", &PlannedTrainingQuestion::question_id);
  Check(RecommendTraining(bank, Summary(), early, {}).question_id == "urgent" &&
            FirstReview(plan).question_id == "urgent" && early_entry != plan.questions.end() &&
            early_entry->purpose == TrainingQuestionPurpose::kEarlyReview,
        "recovery promoted a not-yet-due application into due review");
}

void CheckBudgetFamiliesAndUnchangedInputs() {
  std::vector<Question> cards{Card("application", true)};
  Reviews reviews{{"application", {.due = true, .stage = 5, .seconds_until_due = -1}}};
  for (int index = 0; index < 20; ++index) {
    const auto id = "due-" + std::to_string(index);
    cards.push_back(Card(id));
    reviews.emplace(id, QuestionReviewState{.due = true, .stage = 0, .seconds_until_due = -10000});
  }
  for (int index = 0; index < 12; ++index) {
    cards.push_back(Card("new-" + std::to_string(index)));
  }
  auto variant = Card("application-variant", true);
  variant.question_family_id = cards.front().question_family_id;
  cards.push_back(variant);
  const QuestionBank bank(std::move(cards));
  const auto summary = Summary();
  const auto before = reviews;
  const std::unordered_set<std::string> clean{"unrelated-family"};
  // Repeat to check invariants independently of shuffled equal-priority ties.
  for (int run = 0; run < 20; ++run) {
    const auto plan = PlanRecommendedTraining(bank, summary, reviews, clean);
    Check(plan.questions.size() == 10 && plan.new_question_target == 6 &&
              std::ranges::count(plan.questions, TrainingQuestionPurpose::kNewMaterial,
                                 &PlannedTrainingQuestion::purpose) == 6 &&
              std::ranges::count(plan.questions, TrainingQuestionPurpose::kDueReview,
                                 &PlannedTrainingQuestion::purpose) == 4,
          "recovery displaced the six-new/four-due budget");
    Check(FirstReview(plan).question_id == "application", "recovery application missed the first due slot");
    Check(plan.questions.front().purpose == TrainingQuestionPurpose::kNewMaterial &&
              plan.questions[2].purpose == TrainingQuestionPurpose::kDueReview,
          "recovery changed review interleaving");
    std::unordered_set<std::string> families;
    for (const auto& entry : plan.questions) {
      Check(families.insert(bank.Get(entry.question_id).question_family_id).second,
            "recovery and its variant occupied multiple primary slots");
    }
  }
  Check(clean == std::unordered_set<std::string>{"unrelated-family"} &&
            summary.skills.front().mastery_status == SkillMasteryStatus::kNeedsRecovery,
        "planning changed mastery or families");
  for (const auto& [id, state] : before) {
    const auto& after = reviews.at(id);
    Check(state.due == after.due && state.stage == after.stage && state.seconds_until_due == after.seconds_until_due &&
              state.correct_count == after.correct_count && state.failed_count == after.failed_count,
          "selection advanced a review without an answer");
  }
}

void CheckSafetyAndTopicBalance() {
  auto locked = Card("application", true);
  locked.level = 5;
  const QuestionBank locked_bank({Card("urgent"), locked});
  const auto reviews = CompetingReviews();
  Check(RecommendTraining(locked_bank, Summary(), reviews, {}).question_id == "urgent" &&
            !Contains(PlanRecommendedTraining(locked_bank, Summary(), reviews, {}), "application"),
        "recovery bypassed unlocked difficulty");

  auto application = Card("application", true);
  application.kind = QuestionKind::kImplementation;
  auto urgent = Card("urgent");
  urgent.topic = "containers";
  urgent.skill_id = "containers.map_lookup";
  const QuestionBank filtered_bank({urgent, application});
  for (const auto& selection :
       {QuestionSelection{.topics = {"containers"}}, QuestionSelection{.kinds = {QuestionKind::kTheory}},
        QuestionSelection{.max_level = 1}}) {
    Check(!Contains(PlanRecommendedTraining(filtered_bank, Summary(), reviews, {}, selection), "application"),
          "recovery bypassed explicit topic, format or difficulty filter");
  }

  std::vector<Question> cards{urgent};
  Reviews many{{"urgent", {.due = true, .stage = 0}}};
  for (int index = 0; index < 12; ++index) {
    const auto id = "recovery-" + std::to_string(index);
    cards.push_back(Card(id, true));
    many.emplace(id, QuestionReviewState{.due = true, .stage = 5});
  }
  const QuestionBank balanced_bank(std::move(cards));
  const auto plan = PlanRecommendedTraining(balanced_bank, Summary(), many, {});
  Check(plan.questions.size() == 10 && Contains(plan, "urgent") && FirstReview(plan).question_id != "urgent",
        "recovery either lost first priority or monopolized review topics");
  Check(std::ranges::all_of(plan.questions,
                            [](const auto& entry) { return entry.purpose == TrainingQuestionPurpose::kDueReview; }),
        "due-only recovery invented new material");
}

}  // namespace

void CheckRecoveryPriorityScenarios() {
  CheckRecoveryApplicationFirst();
  CheckFallbacks();
  CheckBudgetFamiliesAndUnchangedInputs();
  CheckSafetyAndTopicBalance();
}
