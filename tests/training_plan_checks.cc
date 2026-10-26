#include "app/quiz_session_service.h"
#include "learning/reference_catalog.h"
#include "learning/theory_catalog.h"
#include "learning/training_recommendation.h"
#include "miniapp/session_snapshot.h"
#include "storage/question_loader.h"

#include <boost/json.hpp>

#include <algorithm>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace {

void Check(bool condition, const char* message) {
  if (!condition) {
    throw std::runtime_error(message);
  }
}

Question Card(std::string id, std::string topic = "language_core", int level = 4) {
  Question result{};
  result.id = std::move(id);
  result.topic = std::move(topic);
  result.skill_id = "core.scope";
  result.question_family_id = "family." + result.id;
  result.level = level;
  result.task = SingleChoiceTask{.options = {{"a", "Correct"}, {"b", "Wrong"}}, .correct_option_id = "a"};
  return result;
}

std::size_t Count(const TrainingPlan& plan, TrainingQuestionPurpose purpose) {
  return static_cast<std::size_t>(std::ranges::count(plan.questions, purpose, &PlannedTrainingQuestion::purpose));
}

UserProgressSummary Summary() {
  return {.adaptive_level = 4,
          .unlocked_level = 5,
          .topics = {{.topic = "language_core", .adaptive_level = 4}, {.topic = "containers", .adaptive_level = 2}},
          .skills = {}};
}

void CheckMixedPlanAndRestoration() {
  std::vector<Question> cards;
  std::unordered_map<std::string, QuestionReviewState> reviews;
  for (int index = 0; index < 20; ++index) {
    const auto id = "due-" + std::to_string(index);
    cards.push_back(Card(id, index == 0 ? "language_core" : "containers", 1));
    reviews.emplace(id, QuestionReviewState{.due = true, .stage = 2, .seconds_until_due = -1000});
  }
  for (int index = 0; index < 12; ++index) {
    cards.push_back(Card("fresh-" + std::to_string(index)));
  }
  auto prerequisite = Card("missing-prerequisite", "templates", 2);
  prerequisite.skill_id = "templates.variadic";
  cards.push_back(prerequisite);
  cards.push_back(Card("too-hard-for-topic", "containers", 4));
  cards.push_back(Card("locked", "language_core", 6));
  QuestionBank bank(std::move(cards));
  const auto plan = PlanRecommendedTraining(bank, Summary(), reviews, {});
  Check(plan.questions.size() == 10 && Count(plan, TrainingQuestionPurpose::kNewMaterial) == 6 &&
            Count(plan, TrainingQuestionPurpose::kDueReview) == 4,
        "a review backlog displaced the reserved new material");
  std::unordered_set<std::string> families, review_topics;
  for (const auto& entry : plan.questions) {
    const auto& question = bank.Get(entry.question_id);
    Check(question.id != "missing-prerequisite" && question.id != "too-hard-for-topic" && question.id != "locked",
          "recommended plan bypassed prerequisites or safe difficulty");
    Check(families.insert(question.question_family_id).second, "one family occupies multiple primary slots");
    if (entry.purpose == TrainingQuestionPurpose::kDueReview) {
      review_topics.insert(question.topic);
    }
  }
  Check(review_topics.size() == 2, "reviews were confined to the largest topic backlog");
  Check(plan.questions.front().purpose == TrainingQuestionPurpose::kNewMaterial &&
            plan.questions[2].purpose == TrainingQuestionPurpose::kDueReview,
        "planned reviews were not distributed through new material");
  Check(reviews.size() == 20 && std::ranges::all_of(reviews, [](const auto& entry) { return entry.second.due; }),
        "planning changed due dates without answers");

  QuizSessionService service(bank);
  QuestionSelection selection;
  selection.max_level = 5;
  auto quiz = service.Start(selection, AdaptiveLevelState{.level = 4}, {}, reviews, plan);
  Check(quiz.queue.PrimaryPending().size() == 3, "balanced sessions no longer load in batches of three");
  const auto failed = service.CurrentQuestion(quiz).id;
  quiz = service.Refill(service.SubmitAnswer(quiz, "b").session);
  Check(service.CurrentQuestion(quiz).id != failed, "an error returned before other primary questions");
  const auto second_failed = service.CurrentQuestion(quiz).id;
  quiz = service.Refill(service.SubmitAnswer(quiz, "b").session);
  quiz = service.Refill(service.SubmitAnswer(quiz, "a").session);
  Check(quiz.queue.PrimaryPending().size() == 3 && quiz.queue.RepairPending().size() == 2,
        "refilling entered repairs before the rest of the mixed plan");

  MiniAppSessionState state{.quiz = quiz,
                            .storage_session_id = 42,
                            .snapshot_version = 1,
                            .modes = {TrainingContentMode::kTheory},
                            .topics = {},
                            .selected_level = 5,
                            .tracks_level_progress = true,
                            .option_order = {"a", "b"},
                            .matching = std::nullopt,
                            .questions_started = 4,
                            .theory_viewed_question_ids = {},
                            .last_question_id = std::nullopt,
                            .last_feedback = std::nullopt};
  const auto snapshot = SerializeMiniAppSessionSnapshot(state);
  quiz = ParseMiniAppSessionSnapshot(snapshot, 42, 1, bank).quiz;
  Check(quiz.training_plan.questions.size() == 10 && quiz.training_plan.new_question_target == 6,
        "mixed plan was lost after restart");
  std::size_t next_index = 3;
  while (!quiz.queue.PrimaryEmpty()) {
    Check(service.CurrentQuestion(quiz).id == plan.questions[next_index++].question_id,
          "restored refill changed the saved question order");
    quiz = service.Refill(service.SubmitAnswer(quiz, "a").session);
  }
  Check(next_index == 10 && service.CurrentQuestion(quiz).id == failed,
        "repair was not placed after the complete primary plan");
  quiz = service.Refill(service.SubmitAnswer(quiz, "a").session);
  Check(service.CurrentQuestion(quiz).id == second_failed, "the second deferred error was lost or reordered");
  quiz = service.Refill(service.SubmitAnswer(quiz, "a").session);
  Check(quiz.queue.Empty() && quiz.completed_question_ids.size() == 10,
        "the mixed session did not complete after repairing its error");

  auto old = boost::json::parse(snapshot).as_object();
  old.at("quiz").as_object().erase("training_plan");
  Check(ParseMiniAppSessionSnapshot(boost::json::serialize(old), 42, 1, bank).quiz.training_plan.questions.empty(),
        "a legacy active session was silently converted to a new plan");
  for (const bool duplicate : {false, true}) {
    auto invalid = boost::json::parse(snapshot).as_object();
    auto& entries = invalid.at("quiz").as_object().at("training_plan").as_object().at("questions").as_array();
    if (duplicate) {
      entries[1] = entries[0];
    } else {
      entries[0].as_object()["purpose"] = "invalid";
    }
    bool rejected = false;
    try {
      static_cast<void>(ParseMiniAppSessionSnapshot(boost::json::serialize(invalid), 42, 1, bank));
    } catch (const std::invalid_argument&) {
      rejected = true;
    }
    Check(rejected, "a corrupt saved training plan was accepted");
  }
}

void CheckSparsePlans() {
  const auto summary = Summary();
  std::vector<Question> fresh;
  for (int index = 0; index < 12; ++index) {
    fresh.push_back(Card("fresh-" + std::to_string(index)));
  }
  Check(
      Count(PlanRecommendedTraining(QuestionBank(fresh), summary, {}, {}), TrainingQuestionPurpose::kNewMaterial) == 10,
      "missing due reviews unnecessarily shortened new learning");
  std::unordered_map<std::string, QuestionReviewState> due;
  for (const auto& question : fresh) {
    due.emplace(question.id, QuestionReviewState{.due = true});
  }
  Check(
      Count(PlanRecommendedTraining(QuestionBank(fresh), summary, due, {}), TrainingQuestionPurpose::kDueReview) == 10,
      "a missing new bank prevented voluntary review");
  fresh.push_back(Card("single-new"));
  const auto scarce = PlanRecommendedTraining(QuestionBank(fresh), summary, due, {});
  Check(Count(scarce, TrainingQuestionPurpose::kNewMaterial) == 1 &&
            Count(scarce, TrainingQuestionPurpose::kDueReview) == 9,
        "unavailable new slots were not filled from due reviews");

  auto seen = Card("seen");
  auto variant = Card("variant");
  variant.question_family_id = seen.question_family_id;
  auto second_variant = variant;
  second_variant.id = "second-variant";
  const auto familiar = PlanRecommendedTraining(QuestionBank({seen, variant, second_variant}), summary,
                                                {{seen.id, {.due = false, .stage = 2}}}, {});
  Check(Count(familiar, TrainingQuestionPurpose::kNewMaterial) == 0 &&
            Count(familiar, TrainingQuestionPurpose::kFamiliarVariant) == 1 && familiar.questions.size() == 1,
        "an unseen variant of a seen family was passed off as new independent material");
  const auto early = PlanRecommendedTraining(QuestionBank({seen}), summary,
                                             {{seen.id, {.due = false, .stage = 2, .seconds_until_due = 86400}}}, {});
  Check(Count(early, TrainingQuestionPurpose::kEarlyReview) == 1, "an early repeat was labelled due or new");

  QuestionBank single_bank({seen});
  QuizSessionService single_service(single_bank);
  const auto only_due = PlanRecommendedTraining(single_bank, summary, {{seen.id, {.due = true}}}, {});
  const auto started = single_service.Start(QuestionSelection{}, AdaptiveLevelState{.level = 4}, {},
                                            {{seen.id, {.due = true}}}, only_due);
  const auto repaired = single_service.Refill(single_service.SubmitAnswer(started, "b").session);
  Check(repaired.session_goal == 1 && repaired.queue.Current() == seen.id && repaired.queue.CurrentAttempts() == 1,
        "the single remaining error was lost or padded with unrelated work");
}

void CheckTemplateLearningPath() {
  auto packs = Card("packs", "templates", 3);
  packs.skill_id = "templates.parameter_packs";
  auto folds = Card("folds", "templates", 4);
  folds.skill_id = "templates.fold_expressions";
  auto empty = Card("empty", "templates", 5);
  empty.skill_id = "templates.variadic";
  QuestionBank bank({packs, folds, empty});
  auto summary = Summary();
  summary.adaptive_level = 5;
  summary.topics.push_back({.topic = "templates", .adaptive_level = 5});
  summary.skills.push_back({.skill_id = "templates.deduction", .mastery_status = SkillMasteryStatus::kDemonstrated});
  const auto contains = [](const TrainingPlan& plan, std::string_view id) {
    return std::ranges::any_of(plan.questions, [id](const auto& entry) { return entry.question_id == id; });
  };
  const auto initial = PlanRecommendedTraining(bank, summary, {}, {});
  Check(contains(initial, "packs") && !contains(initial, "folds") && !contains(initial, "empty"),
        "template deduction alone unlocked fold expressions or empty packs");
  summary.skills.push_back({.skill_id = "templates.parameter_packs", .mastery_status = SkillMasteryStatus::kApplying});
  const auto next = PlanRecommendedTraining(bank, summary, {}, {});
  Check(contains(next, "folds") && !contains(next, "empty"), "pack basics unlocked the empty-pack special case");
  summary.skills.push_back({.skill_id = "templates.fold_expressions", .mastery_status = SkillMasteryStatus::kApplying});
  Check(contains(PlanRecommendedTraining(bank, summary, {}, {}), "empty"),
        "ordinary folds did not unlock the next recommended step");
  summary.skills.back().mastery_status = SkillMasteryStatus::kNeedsRecovery;
  Check(!contains(PlanRecommendedTraining(bank, summary, {}, {}), "empty"),
        "a prerequisite needing recovery unlocked new special cases");

  const auto questions = LoadQuestions("content/questions");
  for (const auto& question : questions) {
    if (question.skill_id != packs.skill_id && question.skill_id != folds.skill_id &&
        question.skill_id != empty.skill_id) {
      continue;
    }
    Check(FindTheoryForSkill(question.skill_id) != nullptr, "template step has no post-error explanation");
    Check(!ReferencesForQuestion(question).empty(), "template step has no pre-answer reference");
    if (question.id == "fold_and_empty_pack_identity") {
      const auto refs = ReferencesForQuestion(question);
      Check(std::ranges::any_of(refs, [](const auto* entry) { return entry->id == "empty_fold"; }),
            "the empty-pack card lost its specific reference");
    }
  }
}

void CheckTopicFilteredPlan() {
  std::vector<Question> cards;
  std::unordered_map<std::string, QuestionReviewState> reviews;
  for (int i = 0; i < 10; ++i) {
    cards.push_back(Card("selected-new-" + std::to_string(i), "containers", 2));
    cards.push_back(Card("other-new-" + std::to_string(i), "language_core", 2));
    auto selected = Card("selected-review-" + std::to_string(i), "containers", 1);
    auto other = Card("other-review-" + std::to_string(i), "language_core", 1);
    reviews.emplace(selected.id, QuestionReviewState{.due = true});
    reviews.emplace(other.id, QuestionReviewState{.due = true});
    cards.push_back(std::move(selected));
    cards.push_back(std::move(other));
  }
  auto excluded = Card("wrong-format", "containers", 1);
  excluded.kind = QuestionKind::kCodeReading;
  cards.push_back(std::move(excluded));
  QuestionBank bank(std::move(cards));
  QuestionSelection selection;
  selection.topics = {"containers"};
  selection.kinds = {QuestionKind::kTheory};
  selection.max_level = 3;
  const auto plan = PlanRecommendedTraining(bank, Summary(), reviews, {}, selection);
  Check(
      Count(plan, TrainingQuestionPurpose::kNewMaterial) == 6 && Count(plan, TrainingQuestionPurpose::kDueReview) == 4,
      "filtered training lost the new/review balance");
  for (const auto& entry : plan.questions) {
    Check(
        bank.Get(entry.question_id).topic == "containers" && bank.Get(entry.question_id).kind == QuestionKind::kTheory,
        "a selected topic or format leaked into new material or reviews");
  }
}

}  // namespace

void CheckTrainingPlans() {
  CheckMixedPlanAndRestoration();
  CheckSparsePlans();
  CheckTopicFilteredPlan();
  CheckTemplateLearningPath();
}
