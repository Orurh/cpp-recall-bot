#include "app/quiz_session_service.h"
#include "learning/learning_route_catalog.h"
#include "learning/reference_catalog.h"
#include "learning/skill_catalog.h"
#include "learning/theory_catalog.h"
#include "learning/training_recommendation.h"
#include "miniapp/session_snapshot.h"
#include "quiz/quiz_service.h"
#include "storage/question_loader.h"

#include <boost/json.hpp>

#include <algorithm>
#include <fstream>
#include <iterator>
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

void CheckLifetimeReferences() {
  const auto questions = LoadQuestions("content/questions");
  const std::unordered_map<std::string, std::string> expected{
      {"pointer_after_local_lifetime", "object_lifetime"},
      {"c_str_from_temporary_dangles", "string_c_str"},
      {"unique_ptr_move_transfers_ownership", "unique_ownership"},
      {"unique_ptr_borrow_without_transfer", "unique_ownership"},
      {"vector_raii_early_return", "scope_cleanup"},
      {"raii_lock_guard_unlocks_during_unwinding", "scope_cleanup"},
  };
  std::size_t checked = 0;
  for (const auto& question : questions) {
    const auto it = expected.find(question.id);
    if (it == expected.end()) {
      continue;
    }
    const auto references = ReferencesForQuestion(question);
    Check(std::ranges::any_of(references, [&it](const auto* entry) { return entry->id == it->second; }),
          "a selected lifetime/RAII card has no matching pre-answer reference");
    ++checked;
  }
  Check(checked == expected.size(), "a selected lifetime/RAII card disappeared");
  std::unordered_set<std::string_view> ids;
  for (const auto& entry : ReferenceCatalog()) {
    Check(ids.insert(entry.id).second, "duplicate reference id");
    Check(!entry.title.empty() && !entry.principle.empty() && !entry.mechanism.empty() && !entry.example.empty() &&
              !entry.common_mistake.empty() && !entry.sources.empty(),
          "reference entry is incomplete");
  }
}

void CheckLifetimeTheoryExamples() {
  std::ifstream input("docs/reviews/theory/2026-10-27-lifetime-examples.json");
  Check(input.is_open(), "lifetime theory example review is missing");
  const std::string text{std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
  const auto document = boost::json::parse(text).as_object();
  Check(document.at("standard_document").as_string() == "N4950" &&
            document.at("reviewer").as_object().at("mode").as_string() == "self_review" &&
            !document.at("reviewer").as_object().at("independent").as_bool(),
        "lifetime theory source review must not claim independent acceptance");
  std::unordered_set<std::string> reviewed;
  for (const auto& value : document.at("reviews").as_array()) {
    const auto& record = value.as_object();
    const std::string skill(record.at("skill_id").as_string());
    const auto* theory = FindTheoryForSkill(skill);
    Check(theory != nullptr && reviewed.insert(skill).second && record.at("id").as_string() == theory->id,
          "unknown or duplicate reviewed theory example");
    Check(record.at("example").as_string() == theory->example,
          "theory example changed: repeat its source and overlap review");
    const auto& sources = record.at("sources").as_array();
    Check(sources.size() == theory->sources.size(), "reviewed theory sources changed");
    for (std::size_t index = 0; index < sources.size(); ++index)
      Check(sources[index].as_string() == theory->sources[index], "theory source diverged from its review");
    Check(!record.at("rationale").as_string().empty() && !record.at("assertions").as_string().empty(),
          "theory example has no review rationale or reproducible assertions");
  }
  Check(reviewed == std::unordered_set<std::string>{"core.storage_duration", "lifetime.objects"},
        "a reviewed lifetime theory example disappeared");
  Check(FindTheoryForSkill("core.storage_duration")->example != FindTheoryForSkill("lifetime.objects")->example,
        "two different prerequisite explanations reused the same example");
}

void CheckContainerLearningRoute() {
  QuestionBank bank(LoadQuestions("content/questions"));
  auto summary = Summary();
  summary.adaptive_level = 8;
  summary.unlocked_level = 8;
  summary.topics = {{.topic = "containers", .adaptive_level = 1}};
  std::unordered_map<std::string, QuestionReviewState> reviews;
  std::unordered_set<std::string> clean;
  auto route = PlanContainerLearningRoute(bank, summary, reviews, clean);
  Check(route.status == "ready" && route.plan.questions.size() == 1 &&
            route.plan.questions.front().question_id == "vector_reserve_does_not_change_size",
        "global OOP progress bypassed container difficulty or blocked its available introduction");
  reviews["vector_reserve_does_not_change_size"] = {.due = false, .correct_count = 1};
  route = PlanContainerLearningRoute(bank, summary, reviews, clean);
  Check(route.status == "difficulty_locked" && route.plan.questions.empty(), "route bypassed topic ceiling");
  summary.topics.front().adaptive_level = 3;
  route = PlanContainerLearningRoute(bank, summary, reviews, clean);
  Check(route.plan.questions.front().question_id == "vector_resize_adds_elements", "route skipped unfinished practice");
  // Introductions completed with help can advance practice, not certify mastery.
  reviews["vector_resize_adds_elements"] = {.due = false, .correct_count = 1};
  route = PlanContainerLearningRoute(bank, summary, reviews, clean);
  Check(route.plan.questions.front().question_id == "find_missing_end_guard" && route.steps[0].completed,
        "help on introductory practice trapped the learner or skipped the search step");
  reviews["find_missing_end_guard"] = {.due = false, .correct_count = 1};
  route = PlanContainerLearningRoute(bank, summary, reviews, clean);
  Check(route.plan.questions.front().question_id == "vector_erase_return_next", "erase introduction was skipped");
  reviews["vector_erase_return_next"] = {.due = false, .correct_count = 1};
  route = PlanContainerLearningRoute(bank, summary, reviews, clean);
  Check(route.plan.questions.front().question_id == "vector_erase_filter_loop", "independent application missing");
  reviews["vector_erase_filter_loop"] = {.due = false, .correct_count = 1};
  route = PlanContainerLearningRoute(bank, summary, reviews, clean);
  Check(route.status == "waiting_review" && !route.steps[3].completed,
        "assisted or corrected answer passed independent application or forced an immediate repeat");
  clean.insert(bank.Get("vector_erase_filter_loop").question_family_id);
  route = PlanContainerLearningRoute(bank, summary, reviews, clean);
  Check(route.status == "waiting_review" && route.steps[3].completed && !route.steps.back().completed,
        "one clean answer was mistaken for delayed retention");
  reviews.at("vector_erase_filter_loop").due = true;
  route = PlanContainerLearningRoute(bank, summary, reviews, clean);
  Check(route.status == "ready" && route.plan.questions.size() == 1 &&
            route.plan.questions.front().purpose == TrainingQuestionPurpose::kDueReview,
        "delayed review was unavailable");
  summary.skills = {{.skill_id = "containers.iterator_invalidation", .mastery_status = SkillMasteryStatus::kRetained}};
  Check(PlanContainerLearningRoute(bank, summary, reviews, clean).status == "completed",
        "existing retained mastery was ignored");
  QuestionBank partial({Card("unrelated")});
  Check(PlanContainerLearningRoute(partial, Summary(), {}, {}).status == "unavailable",
        "incomplete materials caused a fake route or exception");
}

void CheckLearningRouteCatalog() {
  QuestionBank bank(LoadQuestions("content/questions"));
  ValidateLearningRouteCatalog(bank);
  auto invalid_cards = LoadQuestions("content/questions");
  const auto repeated = std::ranges::find(invalid_cards, "map_update_existing_stock", &Question::id);
  repeated->question_family_id = bank.Get("map_insert_or_assign_vs_try_emplace").question_family_id;
  bool rejected = false;
  try {
    ValidateLearningRouteCatalog(QuestionBank(std::move(invalid_cards)));
  } catch (const std::exception&) {
    rejected = true;
  }
  Check(rejected, "a renamed variant was accepted as an independent route stage");
  for (const int defect : {0, 1, 2}) {
    auto changed = LoadQuestions("content/questions");
    const auto final = std::ranges::find(changed, "binary_fold_registration_fee", &Question::id);
    if (defect == 1)
      final->skill_id = "templates.fold_expressions";
    else if (defect == 0)
      final->transfer = false;
    else
      std::ranges::find(changed, "fold_validate_each_measurement", &Question::id)->skill_id = "templates.instantiation";
    bool invalid = false;
    try {
      ValidateLearningRouteCatalog(QuestionBank(std::move(changed)));
    } catch (const std::exception&) {
      invalid = true;
    }
    Check(invalid, "route accepted a guided or unintroduced application or lost its independent retention target");
  }
  Check(LearningRouteCatalog().size() == 5 && FindLearningRoute("unknown") == nullptr,
        "route catalog missing or unknown route accepted");
  auto summary = Summary();
  summary.adaptive_level = summary.unlocked_level = 8;
  std::unordered_map<std::string, QuestionReviewState> reviews;
  auto route = PlanLearningRoute("map_lookup", bank, summary, reviews, {});
  Check(route.plan.questions.size() == 1 && route.plan.questions.front().question_id == "map_find_without_insertion",
        "second route did not use its own introductory stage");
  reviews["map_find_without_insertion"] = {.due = false, .correct_count = 1};
  route = PlanLearningRoute("map_lookup", bank, summary, reviews, {});
  Check(route.plan.questions.size() == 1 &&
            route.plan.questions.front().question_id == "map_subscript_inserts_default_value",
        "partial stage bypassed container difficulty");
  reviews["map_subscript_inserts_default_value"] = {.due = false, .correct_count = 1};
  Check(PlanLearningRoute("map_lookup", bank, summary, reviews, {}).status == "difficulty_locked",
        "second route inherited the global OOP level");
  summary.topics.back().adaptive_level = 3;
  route = PlanLearningRoute("map_lookup", bank, summary, reviews, {});
  Check(route.plan.questions.front().question_id == "map_insert_or_assign_vs_try_emplace", "update practice missing");
  reviews["map_insert_or_assign_vs_try_emplace"] = {.due = false, .correct_count = 1};
  route = PlanLearningRoute("map_lookup", bank, summary, reviews, {});
  Check(route.steps.size() == 4 && route.plan.questions.front().question_id == "map_update_existing_stock",
        "planner still assumes four stages or independent index three");
  reviews["map_update_existing_stock"] = {.due = false, .correct_count = 1};
  Check(PlanLearningRoute("map_lookup", bank, summary, reviews, {}).status == "waiting_review",
        "helped independent application passed without delay");
  const std::unordered_set<std::string> clean{bank.Get("map_update_existing_stock").question_family_id};
  summary.skills = {{.skill_id = "containers.iterator_invalidation", .mastery_status = SkillMasteryStatus::kRetained}};
  Check(PlanLearningRoute("map_lookup", bank, summary, reviews, clean).status == "waiting_review",
        "retention of a different route certified map lookup");
  summary.skills.front().skill_id = "containers.map_lookup";
  Check(PlanLearningRoute("map_lookup", bank, summary, reviews, clean).status == "completed",
        "generic route ignored its target skill");
  for (const auto& definition : LearningRouteCatalog()) {
    for (const auto& stage : definition.stages) {
      for (const auto id : stage.question_ids) {
        Check(!ReferencesForQuestion(bank.Get(id)).empty(), "a route introduces an API without pre-answer help");
      }
    }
  }
}

void CheckLifetimeLearningRoute() {
  QuestionBank bank(LoadQuestions("content/questions"));
  const auto* definition = FindLearningRoute("lifetime_raii");
  Check(definition != nullptr && definition->stages.size() == 8, "lifetime learning sequence is incomplete");
  auto summary = Summary();
  summary.adaptive_level = summary.unlocked_level = 8;
  summary.topics = {{.topic = "language_core", .adaptive_level = 1}, {.topic = "lifetime", .adaptive_level = 1}};
  std::unordered_map<std::string, QuestionReviewState> reviews;
  std::unordered_set<std::string> clean;
  const auto plan = [&] {
    return PlanLearningRoute("lifetime_raii", bank, summary, reviews, clean);
  };
  const auto first_is = [&](std::string_view id) {
    const auto route = plan();
    return route.status == "ready" && !route.plan.questions.empty() && route.plan.questions.front().question_id == id;
  };
  Check(first_is("block_scope_destruction_order"), "lifetime route did not begin with automatic object lifetime");
  reviews["block_scope_destruction_order"] = {.due = false, .correct_count = 1};
  Check(first_is("pointer_reference_share_object"), "valid borrowing was skipped before the dangling-pointer case");
  reviews["pointer_reference_share_object"] = {.due = false, .correct_count = 1};
  Check(plan().status == "difficulty_locked", "a high global level bypassed the separate lifetime ceiling");
  summary.topics.back().adaptive_level = 3;
  Check(first_is("pointer_after_local_lifetime"), "partial introductory stage was lost");
  reviews["pointer_after_local_lifetime"] = {.due = false, .correct_count = 1};
  Check(first_is("vector_raii_early_return"), "return cleanup was skipped");
  reviews["vector_raii_early_return"] = {.due = false, .correct_count = 1};
  Check(first_is("unique_ptr_make_get_observe") && plan().plan.questions.size() == 2,
        "unique_ptr methods appeared before creation and borrowed access were "
        "introduced");
  reviews["unique_ptr_make_get_observe"] = {.due = false, .correct_count = 1};
  Check(first_is("unique_ptr_scope_deletes_object"), "partial ownership introduction was skipped");
  reviews["unique_ptr_scope_deletes_object"] = {.due = false, .correct_count = 1};
  Check(first_is("unique_ptr_move_transfers_ownership"), "move introduction missing");
  reviews["unique_ptr_move_transfers_ownership"] = {.due = false, .correct_count = 1};
  Check(first_is("unique_ptr_reset_destroys_now"), "reset appeared only in independent application");
  reviews["unique_ptr_reset_destroys_now"] = {.due = false, .correct_count = 1};
  Check(plan().status == "difficulty_locked", "lifetime level bypassed the independent language-core ceiling");
  summary.topics.front().adaptive_level = 3;
  Check(first_is("raii_unique_ptr_caught_exception"), "exception cleanup did not use the already introduced owner");
  reviews["block_scope_destruction_order"].due = true;
  const auto mixed_step = plan();
  Check(mixed_step.plan.questions.size() == 2 &&
            mixed_step.plan.questions.back().purpose == TrainingQuestionPurpose::kDueReview,
        "a lifetime step omitted its due review or filled itself with "
        "unrelated material");
  reviews["block_scope_destruction_order"].due = false;
  reviews["raii_unique_ptr_caught_exception"] = {.due = false, .correct_count = 1};
  Check(first_is("unique_ptr_borrow_across_owner_move"), "independent lifetime application missing");
  Check(plan().steps.size() == 9 && clean.empty(), "introductory completion was confused with independent evidence");
  Check(plan().steps.back().title == "Отложенное закрепление: Умные указатели",
        "the route did not name the skill actually checked for retention");
  reviews["unique_ptr_borrow_across_owner_move"] = {.due = false, .correct_count = 1};
  Check(plan().status == "waiting_review" && !plan().steps[7].completed,
        "helped independent answer certified the route or immediately returned");
  reviews["unique_ptr_borrow_across_owner_move"].due = true;
  Check(first_is("unique_ptr_borrow_across_owner_move") &&
            plan().plan.questions.front().purpose == TrainingQuestionPurpose::kDueReview,
        "a due independent retry was not available");
  reviews["unique_ptr_borrow_across_owner_move"].due = false;
  clean.insert(bank.Get("unique_ptr_borrow_across_owner_move").question_family_id);
  Check(plan().status == "waiting_review" && plan().steps[7].completed && !plan().steps.back().completed,
        "one independent answer was mistaken for delayed retention");
  summary.skills = {{.skill_id = "raii.fundamentals", .mastery_status = SkillMasteryStatus::kRetained}};
  Check(plan().status == "waiting_review", "retention of another skill certified ownership");
  summary.skills.front().skill_id = "lifetime.smart_pointers";
  Check(plan().status == "completed", "retained ownership skill was ignored");
  summary.skills.front().mastery_status = SkillMasteryStatus::kNeedsRecovery;
  Check(plan().status != "completed", "ownership needing recovery was reported as retained");
  const auto& exception = bank.Get("raii_unique_ptr_caught_exception");
  Check(exception.question_family_id == bank.Get("raii_lock_guard_unlocks_during_unwinding").question_family_id,
        "a changed resource became a second independent proof of exception "
        "cleanup");
}

void CheckTemplateLearningRoute() {
  QuestionBank bank(LoadQuestions("content/questions"));
  const auto* definition = FindLearningRoute("template_packs_folds");
  Check(definition != nullptr && definition->stages.size() == 6, "template route missing staged applications");
  Check(MasteryPilotSkillIds().size() == 20 && !IsMasteryPilotSkill(definition->retention_skill_id),
        "adding a route silently expanded the assessed skill map");
  auto summary = Summary();
  summary.adaptive_level = summary.unlocked_level = 8;
  summary.topics = {{.topic = "templates", .adaptive_level = 3}};
  std::unordered_map<std::string, QuestionReviewState> reviews;
  std::unordered_set<std::string> clean;
  const auto plan = [&] {
    return PlanLearningRoute(definition->id, bank, summary, reviews, clean);
  };
  const auto first_is = [&](std::string_view id) {
    const auto route = plan();
    return route.status == "ready" && !route.plan.questions.empty() && route.plan.questions.front().question_id == id;
  };
  Check(first_is("parameter_pack_element_count") && plan().plan.questions.size() == 2,
        "empty folds appeared before pack introductions");
  for (const auto id : definition->stages.front().question_ids)
    reviews[std::string(id)] = {.due = false, .correct_count = 1};
  Check(first_is("parameter_pack_frame_arguments"), "pack application missing before fold introduction");
  reviews["parameter_pack_frame_arguments"] = {.due = false, .correct_count = 1};
  Check(plan().status == "waiting_review" && !plan().steps[1].completed,
        "helped pack application unlocked the following stage");
  reviews["parameter_pack_frame_arguments"].due = true;
  Check(first_is("parameter_pack_frame_arguments"), "independent retry did not return when due");
  clean.insert(bank.Get("parameter_pack_frame_arguments").question_family_id);
  reviews["parameter_pack_frame_arguments"].due = false;
  Check(plan().status == "difficulty_locked", "global level bypassed the template topic ceiling");
  summary.topics.front().adaptive_level = 4;
  Check(first_is("fold_and_nonempty_pack"), "fold introduction did not follow pack application");
  for (const auto id : definition->stages[2].question_ids)
    reviews[std::string(id)] = {.due = false, .correct_count = 1};
  Check(first_is("fold_validate_each_measurement"), "fold application missing");
  reviews["fold_validate_each_measurement"] = {.due = false, .correct_count = 1};
  clean.insert(bank.Get("fold_validate_each_measurement").question_family_id);
  Check(plan().status == "difficulty_locked", "special empty-pack rule bypassed its difficulty gate");
  summary.topics.front().adaptive_level = 5;
  Check(first_is("fold_and_empty_pack_identity"), "empty pack introduction missing");
  for (const auto id : definition->stages[4].question_ids)
    reviews[std::string(id)] = {.due = false, .correct_count = 1};
  Check(first_is("binary_fold_registration_fee"), "seeded empty-pack application missing");
  reviews["binary_fold_registration_fee"] = {.due = false, .correct_count = 1};
  Check(plan().status == "waiting_review", "helped final application certified template mastery");
  clean.insert(bank.Get("binary_fold_registration_fee").question_family_id);
  summary.skills = {{.skill_id = "templates.fold_expressions", .mastery_status = SkillMasteryStatus::kRetained}};
  Check(plan().status == "waiting_review", "retention of another template skill completed the route");
  summary.skills.front().skill_id = "templates.variadic";
  Check(plan().status == "completed" && plan().steps.size() == 7,
        "persisted retention outside the pilot map did not complete the route");
  summary.skills.front().mastery_status = SkillMasteryStatus::kNeedsRecovery;
  Check(plan().status != "completed", "recovery was ignored by the template route");
}

void CheckValuesReferencesRoute() {
  QuestionBank bank(LoadQuestions("content/questions"));
  const auto* definition = FindLearningRoute("values_references");
  Check(definition != nullptr && definition->stages.size() == 5, "references route missing staged practice");
  Check(MasteryPilotSkillIds().size() == 20 && !IsMasteryPilotSkill(definition->retention_skill_id),
        "references route expanded the assessed map");
  auto summary = Summary();
  summary.unlocked_level = 5;
  summary.topics = {{.topic = "language_core", .adaptive_level = 1}, {.topic = "references", .adaptive_level = 1}};
  std::unordered_map<std::string, QuestionReviewState> reviews;
  std::unordered_set<std::string> clean;
  const auto plan = [&] {
    return PlanLearningRoute(definition->id, bank, summary, reviews, clean);
  };
  const auto finish_practice = [&](std::size_t stage) {
    const auto route = plan();
    for (const auto id : definition->stages[stage].question_ids) {
      Check(std::ranges::any_of(route.plan.questions, [id](const auto& item) { return item.question_id == id; }),
            "references introduction skipped a card or used another topic ceiling");
      Check(!ReferencesForQuestion(bank.Get(id)).empty(), "references practice has no before-answer help");
      reviews[std::string(id)] = {.due = false, .correct_count = 1};
    }
  };
  finish_practice(0);
  finish_practice(1);
  Check(plan().status == "difficulty_locked", "global level bypassed the references ceiling");
  summary.topics.back().adaptive_level = 2;
  finish_practice(2);
  Check(plan().status == "difficulty_locked", "reference practice bypassed the language-core ceiling");
  summary.topics.front().adaptive_level = 3;
  Check(plan().plan.questions.front().question_id == "value_category_reference_call_assignment",
        "independent category application missing");
  reviews["value_category_reference_call_assignment"] = {.due = false, .correct_count = 1};
  Check(plan().status == "waiting_review" && !plan().steps[3].completed,
        "assisted category application unlocked a later stage or immediately repeated");
  reviews.at("value_category_reference_call_assignment").due = true;
  Check(plan().plan.questions.front().purpose == TrainingQuestionPurpose::kDueReview,
        "due category retry was not available");
  clean.insert(bank.Get("value_category_reference_call_assignment").question_family_id);
  reviews.at("value_category_reference_call_assignment").due = false;
  Check(plan().status == "difficulty_locked", "language-core ceiling bypassed reference return-type difficulty");
  summary.topics.back().adaptive_level = 3;
  const auto& completion = bank.Get("reference_return_existing_member");
  Check(plan().plan.questions.front().question_id == completion.id, "reference return completion missing");
  for (const auto answer : {"int&", " int  & ", "int\n&", "signed int &", "int signed&", "signed&"})
    Check(CheckAnswer(completion, answer) == AnswerResult::kCorrect, "type completion rejected whitespace");
  for (const auto answer : {"int", "int&&", "const int&", "int*", "return int&;", "int&;", "auto&"})
    Check(CheckAnswer(completion, answer) == AnswerResult::kIncorrect, "type completion accepted wrong tokens");
  reviews[completion.id] = {.due = false, .correct_count = 1};
  Check(plan().status == "waiting_review" && !plan().steps[4].completed,
        "assisted completion certified reference knowledge");
  clean.insert(completion.question_family_id);
  Check(plan().steps[4].completed && !plan().steps.back().completed, "clean completion certified delayed retention");
  reviews.at(completion.id).due = true;
  Check(plan().plan.questions.front().question_id == completion.id &&
            plan().plan.questions.front().purpose == TrainingQuestionPurpose::kDueReview,
        "completed practice excluded a due retention check");
  summary.skills = {{.skill_id = "core.value_categories", .mastery_status = SkillMasteryStatus::kRetained}};
  Check(plan().status != "completed", "retained categories certified the reference target");
  summary.skills.front().skill_id = "references.fundamentals";
  Check(plan().status == "completed" && plan().steps.size() == 6, "persisted reference retention was ignored");
  summary.skills.front().mastery_status = SkillMasteryStatus::kNeedsRecovery;
  Check(plan().status != "completed", "reference recovery was ignored");
  for (const auto skill : {"core.value_categories", "references.fundamentals"})
    Check(FindTheoryForSkill(skill) != nullptr, "references route has no after-error explanation");
}

void CheckTemplateImplementationAnswers() {
  QuestionBank bank(LoadQuestions("content/questions"));
  const auto& predicate = bank.Get("fold_validate_each_measurement");
  const auto& fee = bank.Get("binary_fold_registration_fee");
  for (const auto answer : {"(... && in_range ( values ))", "return(in_range(values)&&...);"})
    Check(CheckAnswer(predicate, answer) == AnswerResult::kCorrect, "predicate fold rejected valid whitespace");
  for (const auto answer : {"(... && values)", "(in_range(values) || ...)", "(in_range(values) && . . .)"})
    Check(CheckAnswer(predicate, answer) == AnswerResult::kIncorrect, "predicate fold accepted incorrect tokens");
  Check(CheckAnswer(fee, "return ( base + ... + extras );") == AnswerResult::kCorrect,
        "seeded fold rejected a full return statement");
  for (const auto answer : {"(... + extras)", "((base + extras) + ...)", "(base + extras...)"})
    Check(CheckAnswer(fee, answer) == AnswerResult::kIncorrect, "seeded fold accepted incorrect expression");
}

void CheckContainerReferences() {
  const auto questions = LoadQuestions("content/questions");
  const std::unordered_map<std::string, std::vector<std::string_view>> expected{
      {"vector_reserve_does_not_change_size", {"vector_storage"}},
      {"vector_invalidation_multiple_choice", {"vector_iterators"}},
      {"erase_remove_vector_size", {"remove_erase"}},
      {"stl_algorithms_matching", {"lower_bound", "remove_erase"}},
      {"vector_resize_adds_elements", {"vector_storage"}},
      {"find_missing_end_guard", {"iterator_range"}},
      {"vector_erase_return_next", {"iterator_range", "vector_iterators"}},
  };
  for (const auto& [id, references] : expected) {
    const auto it = std::ranges::find(questions, id, &Question::id);
    Check(it != questions.end(), "container reference fixture missing");
    const auto actual = ReferencesForQuestion(*it);
    for (const auto reference : references) {
      Check(std::ranges::any_of(actual, [reference](const auto* entry) { return entry->id == reference; }),
            "container card is missing a required pre-answer reference");
    }
  }
  for (const auto id : {"flat_map_min_dangling_reference", "unordered_map_reserve_vs_rehash"}) {
    const auto it = std::ranges::find(questions, id, &Question::id);
    Check(it != questions.end(), "non-vector fixture missing");
    const auto references = ReferencesForQuestion(*it);
    Check(std::ranges::none_of(
              references,
              [](const auto* entry) { return entry->id == "vector_iterators" || entry->id == "vector_storage"; }),
          "vector-only guarantees leaked to a different container through generic tags");
  }
}

void CheckProjectedKeyCompletion() {
  const auto questions = LoadQuestions("content/questions");
  const auto it = std::ranges::find(questions, "sorted_snapshot_key_alias_completion", &Question::id);
  Check(it != questions.end(), "projected key completion card missing");
  Check(CheckAnswer(*it, " std::remove_cvref_t < std::invoke_result_t < const KeyOf & , const T & > > ") ==
            AnswerResult::kCorrect,
        "alias completion rejected valid whitespace or separated closing brackets");
  Check(CheckAnswer(*it, "using Key=std::remove_cvref_t<std::invoke_result_t<const KeyOf&,const T&>>;") ==
            AnswerResult::kCorrect,
        "alias completion rejected the full declaration");
  for (const auto answer : {"std::invoke_result_t<const KeyOf&, const T&>", "std::remove_cvref_t<T>",
                            "std::remove_cvref_t<std::invoke_result_t<KeyOf, const T&>>",
                            "std::remove_cvref_t<std::invoke_result_t<const KeyOf&, T&>>"}) {
    Check(CheckAnswer(*it, answer) == AnswerResult::kIncorrect, "alias completion accepted the wrong type request");
  }
  const auto references = ReferencesForQuestion(*it);
  for (const auto id : {"invoke_result", "remove_cvref"}) {
    Check(std::ranges::any_of(references, [id](const auto* entry) { return entry->id == id; }),
          "alias completion is missing a required reference");
  }
  QuestionBank bank(questions);
  const auto& name = bank.Get("projected_name_key_concrete_type");
  const auto& pointer = bank.Get("projected_pointer_key_concrete_type");
  Check(name.question_family_id == it->question_family_id && pointer.question_family_id == it->question_family_id,
        "projected-key variants became independent families");
  Check(CheckAnswer(name, " std :: string ") == AnswerResult::kCorrect &&
            CheckAnswer(pointer, "int const *") == AnswerResult::kCorrect,
        "concrete key types rejected equivalent spelling or whitespace");
  for (const auto answer : {"int*", "const int* const", "const int*&"})
    Check(CheckAnswer(pointer, answer) == AnswerResult::kIncorrect,
          "key type lost nested const or retained a reference");
  for (const auto answer : {"std::string&", "const std::string", "std::string*"})
    Check(CheckAnswer(name, answer) == AnswerResult::kIncorrect, "key type retained outer qualifiers");
  auto summary = Summary();
  summary.unlocked_level = 6;
  summary.topics = {{.topic = "templates", .adaptive_level = 6}};
  summary.skills = {{.skill_id = "templates.deduction", .mastery_status = SkillMasteryStatus::kDemonstrated}};
  QuestionBank variants({*it, name, pointer});
  const auto plan = PlanRecommendedTraining(variants, summary, {{it->id, {.due = false, .correct_count = 1}}},
                                            {it->question_family_id});
  Check(plan.questions.size() == 1 && plan.questions.front().question_id != it->id &&
            Count(plan, TrainingQuestionPurpose::kNewMaterial) == 0,
        "unseen key variants were ignored or counted as new independent material");
}

void CheckDesignAndSfinaePrerequisites() {
  QuestionBank full(LoadQuestions("content/questions"));
  for (const auto id :
       {"srp_actor_not_method_count", "srp_grading_and_public_ranking", "srp_related_operations_one_actor",
        "ocp_extend_selected_variation", "lsp_override_does_not_prove_contract", "isp_read_client_narrow_contract",
        "dip_injection_is_not_abstraction", "sfinae_substitution_not_any_error", "sfinae_trailing_return_fallback",
        "sfinae_enable_if_integral_condition", "sfinae_body_error_is_hard_error"}) {
    const auto& card = full.Get(id);
    Check(!ReferencesForQuestion(card).empty(), "a new design or SFINAE card has no pre-answer help");
    Check(!IsMasteryPilotSkill(card.skill_id), "new content silently expanded the assessed map");
    if (card.topic == "oop")
      Check(card.standard == "Принципы проектирования", "SOLID was labelled as a C++ standard rule");
    else
      Check(card.level >= 5 && FindTheoryForSkill(card.skill_id) != nullptr,
            "SFINAE appeared without advanced scaffolding");
  }
  Check(MasteryPilotSkillIds().size() == 20 && FindTheoryForSkill("oop.srp") != nullptr,
        "SRP theory missing or skill map denominator changed");
  auto summary = Summary();
  summary.unlocked_level = 6;
  summary.topics = {{.topic = "templates", .adaptive_level = 6}};
  summary.skills = {{.skill_id = "templates.deduction", .mastery_status = SkillMasteryStatus::kDemonstrated},
                    {.skill_id = "templates.instantiation", .mastery_status = SkillMasteryStatus::kDemonstrated}};
  const auto& sfinae = full.Get("sfinae_substitution_not_any_error");
  QuestionBank bank({sfinae, Card("safe-control", "language_core", 2)});
  const auto has_sfinae = [&] {
    const auto plan = PlanRecommendedTraining(bank, summary, {}, {});
    return std::ranges::any_of(plan.questions, [&](const auto& entry) { return entry.question_id == sfinae.id; });
  };
  Check(!has_sfinae(), "SFINAE bypassed type-trait prerequisites");
  summary.skills.push_back({.skill_id = "templates.type_traits", .mastery_status = SkillMasteryStatus::kApplying});
  Check(has_sfinae(), "prepared SFINAE question remained locked");
  summary.topics.front().adaptive_level = 4;
  Check(!has_sfinae(), "SFINAE bypassed the topic difficulty ceiling");
  summary.topics.front().adaptive_level = 6;
  summary.skills.back().mastery_status = SkillMasteryStatus::kNeedsRecovery;
  Check(!has_sfinae(), "a prerequisite needing recovery unlocked SFINAE");
  const auto& completion = full.Get("sfinae_enable_if_integral_condition");
  Check(CheckAnswer(completion, "std :: is_integral_v < T >") == AnswerResult::kCorrect &&
            CheckAnswer(completion, "std::is_arithmetic_v<T>") == AnswerResult::kIncorrect,
        "enable_if completion lost token checking");
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
  CheckDesignAndSfinaePrerequisites();
  CheckMixedPlanAndRestoration();
  CheckSparsePlans();
  CheckTopicFilteredPlan();
  CheckTemplateLearningPath();
  CheckLifetimeReferences();
  CheckLifetimeTheoryExamples();
  CheckContainerReferences();
  CheckContainerLearningRoute();
  CheckLearningRouteCatalog();
  CheckLifetimeLearningRoute();
  CheckTemplateLearningRoute();
  CheckValuesReferencesRoute();
  CheckTemplateImplementationAnswers();
  CheckProjectedKeyCompletion();
}
