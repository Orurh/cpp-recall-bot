#include "app/quiz_session_service.h"
#include "app/training_mode.h"
#include "bot/card_renderer.h"
#include "learning/adaptive_level.h"
#include "learning/learning_queue.h"
#include "learning/level_progress_policy.h"
#include "learning/module_catalog.h"
#include "learning/progress_summary.h"
#include "learning/review_schedule.h"
#include "learning/review_state.h"
#include "learning/skill_catalog.h"
#include "learning/skill_mastery.h"
#include "learning/theory_catalog.h"
#include "learning/training_recommendation.h"
#include "miniapp/session_snapshot.h"
#include "miniapp/shared_challenge.h"
#include "miniapp/telegram_init_data.h"
#include "monitoring/runtime_metrics.h"
#include "quiz/question.h"
#include "quiz/question_bank.h"
#include "quiz/question_identity.h"
#include "quiz/question_validator.h"
#include "quiz/quiz_service.h"
#include "quiz/topic_catalog.h"
#include "runtime/bounded_executor.h"
#include "runtime/keyed_mutex.h"
#include "storage/question_loader.h"

#include <boost/json.hpp>

#include <openssl/evp.h>
#include <openssl/hmac.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <climits>
#include <future>
#include <iostream>
#include <limits>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

void CheckContentReviews();
void CheckReferenceBindings();
void CheckTrainingPlans();
void CheckRecoveryPriorityScenarios();
void CheckCppCurriculum();

namespace {

void Require(bool condition, std::string_view message) {
  if (!condition) {
    throw std::runtime_error(std::string(message));
  }
}

void TestTheoryHelpSelection() {
  const auto* storage = FindTheoryForSkill("core.storage_duration");
  Require(storage && storage->id == "storage_duration.basics", "default storage help changed");
  const auto* order = FindTheoryForQuestion("core.storage_duration", "core.storage_duration.initialization_order");
  Require(order && order->id == "storage_duration.initialization_order" && !order->primary,
          "initialization order was routed to basic storage help");
  const auto* copy = FindTheoryForSkill("special_members.copy_construction");
  Require(copy && copy->id == "copy_construction.object_lifecycle", "copy construction has no beginner help");
  const TheoryBlock* map_contract = nullptr;
  for (const auto family :
       {"containers.map_lookup.map_lookup_methods_matching",
        "containers.map_lookup.map_subscript_inserts_default_value", "containers.map_lookup.nonmutating_search",
        "containers.map_lookup.explicit_update_methods", "containers.map_lookup.conditional_stock_update"}) {
    const auto* theory = FindTheoryForQuestion("containers.map_lookup", family);
    Require(theory && theory->id == "qa20261124.map_contract_checklist" && !theory->primary,
            "basic map family lost its operation contract help");
    Require(map_contract == nullptr || map_contract == theory, "basic map families use different contract help");
    map_contract = theory;
  }
  Require(
      FindTheoryForSkill("containers.map_lookup") == nullptr &&
          FindTheoryForQuestion("containers.map_lookup", "") == nullptr &&
          FindTheoryForQuestion("containers.map_lookup", "unreviewed-map-family") == nullptr &&
          FindTheoryForQuestion("containers.map_lookup", "containers.map_lookup.map_transparent_lookup") == nullptr &&
          FindTheoryForQuestion("unknown-skill", "containers.map_lookup.nonmutating_search") == nullptr,
      "map contract help leaked beyond its five reviewed families");
  const auto* node = FindTheoryForQuestion("containers.map_lookup", "containers.map_lookup.map_node_handle_change_key");
  Require(node && node != map_contract && node->id == "qa20261125.node_ownership_principle" && !node->primary,
          "reviewed node ownership family lost its separate targeted help");
  const auto* forwarding =
      FindTheoryForQuestion("templates.deduction", "templates.deduction.forwarding_reference_lvalue_deduction");
  Require(forwarding && forwarding->id == "deduction.forwarding_reference" && !forwarding->primary,
          "forwarding reference family has no targeted introduction");
  Require(FindTheoryForSkill("templates.deduction") == nullptr &&
              FindTheoryForQuestion("templates.deduction", "templates.deduction.template_by_value_drops_top_const") ==
                  nullptr &&
              FindTheoryForQuestion("templates.deduction", "templates.deduction.decltype_auto_parenthesized_return") ==
                  nullptr &&
              FindTheoryForQuestion("templates.deduction", "") == nullptr &&
              FindTheoryForQuestion("references.fundamentals",
                                    "templates.deduction.forwarding_reference_lvalue_deduction") != forwarding,
          "forwarding help leaked to unrelated deduction or reference families");
  Require(FindTheoryForQuestion("core.storage_duration", "other-family") == storage &&
              FindTheoryForQuestion("core.storage_duration", "") == storage &&
              FindTheoryForQuestion("special_members.copy_construction",
                                    "core.storage_duration.initialization_order") == copy &&
              FindTheoryForQuestion("unknown-skill", "core.storage_duration.initialization_order") == nullptr,
          "unrelated or cross-skill family inherited specialized help");
  ValidateTheoryCatalog();
  const std::vector<TheoryBinding> valid{
      {"core.storage_duration", "core.storage_duration.initialization_order", "storage_duration.initialization_order"}};
  ValidateTheoryBindings(TheoryCatalog(), valid);
  const auto reject = [](auto&& operation) {
    bool rejected = false;
    try {
      operation();
    } catch (const std::logic_error&) {
      rejected = true;
    }
    Require(rejected, "invalid theory binding passed validation");
  };
  for (const auto target : {"missing-theory", "copy_construction.object_lifecycle", ""}) {
    auto invalid = valid;
    invalid.front().theory_id = target;
    reject([&] { ValidateTheoryBindings(TheoryCatalog(), invalid); });
  }
  auto duplicate = valid;
  duplicate.push_back(valid.front());
  reject([&] { ValidateTheoryBindings(TheoryCatalog(), duplicate); });
  auto blocks = std::vector<TheoryBlock>(TheoryCatalog().begin(), TheoryCatalog().end());
  blocks.push_back(*order);
  reject([&] { ValidateTheoryBindings(blocks, valid); });
}

Question MakeQuestion(std::string id, std::string topic, int level) {
  return {
      .id = id,
      .topic = topic,
      .skill_id = "core.scope",
      .question_family_id = "core.scope." + id,
      .evidence_type = QuestionEvidenceType::kRecognition,
      .transfer = false,
      .kind = QuestionKind::kTheory,
      .level = level,
      .standard = "C++23",
      .tags = {"test"},
      .text = "Test question <&>",
      .code = std::nullopt,
      .task =
          SingleChoiceTask{
              .options = {{.id = "a", .text = "Correct"}, {.id = "b", .text = "Wrong"}},
              .correct_option_id = "a",
          },
      .explanation = "Test explanation",
      .source = std::nullopt,
  };
}

void TestLearningQueue() {
  LearningQueue queue({"first", "second"});
  Require(queue.Current() == "first", "queue must start with first question");

  const auto first = queue.SubmitAnswer(false);
  Require(first.attempts == 1 && !first.completed, "first wrong answer must requeue question");
  Require(queue.Current() == "second", "requeued question must move to back");

  queue.AppendPrimary({"third"});
  static_cast<void>(queue.SubmitAnswer(true));
  Require(queue.Current() == "third", "new primary questions must remain ahead of repairs");
  static_cast<void>(queue.SubmitAnswer(true));
  Require(queue.Current() == "first", "requeued question must return after primary questions");
  const auto second_error = queue.SubmitAnswer(false);
  Require(second_error.attempts == 2 && !second_error.completed,
          "a repeated wrong answer must not complete a learning card");
  const auto corrected = queue.SubmitAnswer(true);
  Require(corrected.attempts == 3 && corrected.completed,
          "a learning card must complete after the learner finally answers correctly");
  Require(queue.Empty(), "queue must be empty after completed questions");

  LearningQueue restored({{.question_id = "new"}}, {{.question_id = "repair", .attempts = 2}},
                         LearningQueue::RestoreStateTag{});
  Require(restored.Current() == "new", "restored primary work must remain ahead of repairs");
  static_cast<void>(restored.SubmitAnswer(true));
  Require(restored.Current() == "repair" && restored.CurrentAttempts() == 2,
          "restored repair attempts must be preserved");
}

void TestQuestionIdentity() {
  Require(IsPublicQuestionId("CR-000001") && IsPublicQuestionId("CR-999999"), "valid public card number rejected");
  Require(!IsPublicQuestionId("CR-000000") && !IsPublicQuestionId("CR-123") && !IsPublicQuestionId("CR-00000a"),
          "invalid public card number accepted");
  const auto first = CanonicalQuestionJson(boost::json::parse(R"({"text":"A", "task":{"answers":[1,2],"key":1}})"));
  const auto reordered = CanonicalQuestionJson(boost::json::parse(R"({"task":{"key":1,"answers":[1,2]},"text":"A"})"));
  const auto changed = CanonicalQuestionJson(boost::json::parse(R"({"text":"A", "task":{"answers":[2,1],"key":1}})"));
  Require(QuestionContentRevision(first) == QuestionContentRevision(reordered),
          "formatting or object member order changed a card revision");
  Require(QuestionContentRevision(first) != QuestionContentRevision(changed),
          "changing answer order did not change the card revision");
  Require(QuestionContentRevision("abc") == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad",
          "card revision is not the expected SHA-256 digest");
}

void TestMiniAppSessionSnapshot() {
  QuestionBank bank({MakeQuestion("snapshot-question", "core", 2)});
  QuizSessionService service(bank);
  MiniAppSessionState session{
      .quiz = service.Start(
          QuestionSelection{
              .topics = {},
              .skill_ids = {},
              .kinds = {},
              .matching_only = false,
              .min_level = 2,
              .max_level = 2,
              .preferred_question_ids = {},
          },
          AdaptiveLevelState{.level = 2, .evidence = 1}, {}, {}),
      .storage_session_id = 42,
      .snapshot_version = 7,
      .modes = {TrainingContentMode::kTheory},
      .topics = {"core"},
      .selected_level = 2,
      .tracks_level_progress = true,
      .option_order = {"b", "a"},
      .matching = std::nullopt,
      .questions_started = 1,
      .current_attempt = 1,
      .xp_gained = 12,
      .theory_viewed_question_ids = {"snapshot-question"},
      .last_question_id = std::nullopt,
      .last_feedback = std::nullopt,
  };

  session.reference_viewed_question_ids = {"snapshot-question"};
  session.quiz.adaptive_state.promotion = {.level = 2, .families = {{"snapshot.clean", true}}};
  session.quiz.topic_states["core"] = {.level = 2, .promotion = {.level = 2, .families = {{"snapshot.topic", false}}}};
  const auto restored = ParseMiniAppSessionSnapshot(SerializeMiniAppSessionSnapshot(session), 42, 7, bank);
  Require(restored.storage_session_id == 42 && restored.snapshot_version == 7,
          "session snapshot identity was not restored");
  Require(restored.quiz.queue.Current() == "snapshot-question" && restored.option_order == session.option_order,
          "session snapshot did not preserve the current question presentation");
  Require(
      restored.quiz.adaptive_state.level == 2 && restored.quiz.adaptive_state.evidence == 1 && restored.xp_gained == 12,
      "session snapshot did not preserve learning state");
  Require(restored.theory_viewed_question_ids == session.theory_viewed_question_ids,
          "session snapshot did not preserve opened theory state");
  Require(restored.reference_viewed_question_ids == session.reference_viewed_question_ids,
          "session snapshot did not preserve reference assistance");
  Require(restored.quiz.adaptive_state.promotion == session.quiz.adaptive_state.promotion &&
              restored.quiz.topic_states.at("core").promotion == session.quiz.topic_states.at("core").promotion,
          "session snapshot lost distinct family evidence or application provenance");
  auto legacy_snapshot = boost::json::parse(SerializeMiniAppSessionSnapshot(session)).as_object();
  legacy_snapshot.erase("reference_viewed_question_ids");
  legacy_snapshot.erase("assisted_first_try_correct");
  legacy_snapshot.at("quiz").as_object().at("adaptive_state").as_object().erase("promotion");
  for (auto& topic : legacy_snapshot.at("quiz").as_object().at("topic_states").as_array())
    topic.as_object().at("state").as_object().erase("promotion");
  const auto legacy_restored = ParseMiniAppSessionSnapshot(boost::json::serialize(legacy_snapshot), 42, 7, bank);
  Require(legacy_restored.reference_viewed_question_ids.empty() && legacy_restored.assisted_first_try_correct == 0,
          "pre-reference snapshots are no longer compatible");
  Require(legacy_restored.quiz.adaptive_state.promotion.families.empty() &&
              legacy_restored.quiz.topic_states.at("core").promotion.families.empty(),
          "old scalar-only snapshots invented promotion families");

  auto wrong_level = boost::json::parse(SerializeMiniAppSessionSnapshot(session)).as_object();
  wrong_level.at("quiz").as_object().at("adaptive_state").as_object()["level"] = 3;
  bool rejected_family_level = false;
  try {
    static_cast<void>(ParseMiniAppSessionSnapshot(boost::json::serialize(wrong_level), 42, 7, bank));
  } catch (const std::invalid_argument&) {
    rejected_family_level = true;
  }
  Require(rejected_family_level, "snapshot reused family evidence after a level change");

  auto incompatible = boost::json::parse(SerializeMiniAppSessionSnapshot(session)).as_object();
  incompatible["option_order"] = boost::json::array{"missing"};
  bool rejected = false;
  try {
    static_cast<void>(ParseMiniAppSessionSnapshot(boost::json::serialize(incompatible), 42, 7, bank));
  } catch (const std::invalid_argument&) {
    rejected = true;
  }
  Require(rejected, "an incompatible question presentation snapshot was accepted");

  session.quiz.answered = true;
  session.last_question_id = "snapshot-question";
  session.last_feedback = boost::json::object{{"correct", false},
                                              {"answer_revealed", true},
                                              {"correct_answer", "pinned answer"},
                                              {"explanation", "pinned explanation"}};
  const auto disclosed = ParseMiniAppSessionSnapshot(SerializeMiniAppSessionSnapshot(session), 42, 7, bank);
  Require(disclosed.last_feedback == session.last_feedback &&
              disclosed.theory_viewed_question_ids == session.theory_viewed_question_ids,
          "saved feedback forgot the explicit disclosure or its assistance");
}

void TestBugHunterValidation() {
  const auto cards = LoadQuestions("content/questions");
  const auto card = std::ranges::find(cards, "vector_reserve_bug_line", &Question::id);
  Require(card != cards.end(), "missing mobile bug card");
  const auto previous = std::ranges::find(cards, "vector_reserve_does_not_change_size", &Question::id);
  Require(previous != cards.end() && card->question_family_id == previous->question_family_id &&
              card->evidence_type == QuestionEvidenceType::kRecognition && !card->transfer,
          "line selection invented independent application evidence");
  Require(CheckAnswer(*card, "line_6") == AnswerResult::kCorrect &&
              CheckAnswer(*card, "line_4") == AnswerResult::kIncorrect &&
              CheckAnswer(*card, "line_5") == AnswerResult::kIncorrect &&
              CheckAnswer(*card, "line_06") == AnswerResult::kUnknownOption,
          "line IDs were not checked by the stock answer checker");
  const auto reject = [](const Question& changed) {
    bool rejected = false;
    try {
      ValidateQuestion(changed);
    } catch (const std::invalid_argument&) {
      rejected = true;
    }
    Require(rejected, "malformed bug_hunter_line markup was accepted");
  };
  for (const auto id : {"line_0", "line_04", "line_-1", "line_+4", "line_4x", "line_8", "line_2",
                        "line_184467440737095516160", "arbitrary"}) {
    auto changed = *card;
    auto& task = std::get<SingleChoiceTask>(changed.task);
    task.options.front().id = id;
    reject(changed);
  }
  auto changed = *card;
  std::get<SingleChoiceTask>(changed.task).options.front().text = "Строка 4: другой код";
  reject(changed);
  changed = *card;
  changed.kind = QuestionKind::kCodeReading;
  reject(changed);
  changed = *card;
  changed.code.reset();
  reject(changed);
  changed = *card;
  changed.task = ShortAnswerTask{.accepted_answers = {"line_6"}};
  reject(changed);
  changed = *card;
  std::get<SingleChoiceTask>(changed.task).options.resize(1);
  reject(changed);
}

void TestAnswerChecking() {
  auto single = MakeQuestion("single", "core", 1);
  Require(CheckAnswer(single, "a") == AnswerResult::kCorrect, "single-choice correct answer was rejected");
  Require(CheckAnswer(single, "missing") == AnswerResult::kUnknownOption, "unknown option was not detected");

  Question multiple = MakeQuestion("multiple", "core", 1);
  multiple.task = MultipleChoiceTask{
      .options = {{.id = "a", .text = "A"}, {.id = "b", .text = "B"}, {.id = "c", .text = "C"}},
      .correct_option_ids = {"a", "c"},
  };
  const std::vector<std::string> selected{"c", "a"};
  Require(CheckAnswer(multiple, selected) == AnswerResult::kCorrect,
          "multiple-choice answer must be order-independent");

  Question short_answer = MakeQuestion("short", "core", 1);
  short_answer.task = ShortAnswerTask{.accepted_answers = {"std::move"}};
  Require(CheckAnswer(short_answer, "  std::move\n") == AnswerResult::kCorrect,
          "short answer must ignore surrounding whitespace");
  Require(CheckAnswer(short_answer, "std:: move") == AnswerResult::kIncorrect,
          "ordinary short answer must preserve meaningful internal whitespace");

  Question implementation = MakeQuestion("implementation", "containers", 4);
  implementation.question_family_id = "core.scope.implementation";
  implementation.evidence_type = QuestionEvidenceType::kApplication;
  implementation.kind = QuestionKind::kImplementation;
  implementation.task = ShortAnswerTask{.accepted_answers = {"&it->second.value"}};
  Require(CheckAnswer(implementation, "  & it -> second . value\n") == AnswerResult::kCorrect,
          "implementation answer must ignore whitespace between C++ tokens");
  for (const auto answer : {"&((it->second).value)", "(&(((it)->second).value))"}) {
    Require(CheckAnswer(implementation, answer) == AnswerResult::kCorrect,
            "member expression must accept redundant grouping");
  }
  for (const auto answer : {"(&it)->second.value", "&it->second.value;", "&it->second.value + 1", "&(it->second.value",
                            "&it->second.value)"}) {
    Require(CheckAnswer(implementation, answer) == AnswerResult::kIncorrect,
            "member normalization must preserve precedence, boundaries and the requested expression");
  }
  const std::string too_deep = "&" + std::string(128, '(') + "it->second.value" + std::string(128, ')');
  Require(CheckAnswer(implementation, too_deep) == AnswerResult::kIncorrect,
          "member parser must bound recursive grouping");

  implementation.task = ShortAnswerTask{.accepted_answers = {"return 1;"}};
  for (const auto& continuation : {std::string("\\\n"), std::string("\\ \t\r\n"), std::string("\\\r")}) {
    const auto hidden = "// continued " + continuation + "return 1;";
    Require(CheckAnswer(implementation, hidden) == AnswerResult::kIncorrect,
            "continued line comment must not expose hidden answer tokens");
  }
  Require(CheckAnswer(implementation, "// ordinary comment\nreturn 1;") == AnswerResult::kCorrect,
          "ordinary line comments must still be supported");

  implementation.task = ShortAnswerTask{.accepted_answers = {"return \"a b\";", "value + +offset"}};
  Require(CheckAnswer(implementation, "return \"ab\";") == AnswerResult::kIncorrect,
          "implementation answer must preserve whitespace inside a string literal");
  Require(CheckAnswer(implementation, "value++offset") == AnswerResult::kIncorrect,
          "implementation answer must distinguish separate punctuators from one operator");
  Require(CheckAnswer(implementation, " return /* formatting */ \"a b\" ; ") == AnswerResult::kCorrect,
          "implementation answer may ignore formatting and comments outside literals");

  implementation.task = ShortAnswerTask{.accepted_answers = {"return obj.*ptr;"}};
  Require(CheckAnswer(implementation, "return obj.*ptr;") == AnswerResult::kCorrect,
          "implementation answer must recognize the pointer-to-member operator");
  Require(CheckAnswer(implementation, "return obj. *ptr;") == AnswerResult::kIncorrect,
          "implementation answer must not split one pointer-to-member operator into two tokens");
  implementation.task = ShortAnswerTask{.accepted_answers = {"const int*"}};
  Require(CheckAnswer(implementation, "(const int*)") == AnswerResult::kIncorrect,
          "expression grouping normalization must not apply to a type-id");
  implementation.task = ShortAnswerTask{.accepted_answers = {"a + b * c"}};
  Require(CheckAnswer(implementation, "(a + b) * c") == AnswerResult::kIncorrect,
          "grouping must never be erased from arbitrary arithmetic");

  implementation.task = ShortAnswerTask{.accepted_answers = {"#define VALUE 1\nreturn VALUE;"}};
  Require(CheckAnswer(implementation, "#define VALUE 1\nreturn VALUE;") == AnswerResult::kIncorrect,
          "short implementation answers must reject preprocessing directives");
  implementation.task = ShortAnswerTask{.accepted_answers = {"%:define VALUE 1\nreturn VALUE;"}};
  Require(CheckAnswer(implementation, "%:define VALUE 1\nreturn VALUE;") == AnswerResult::kIncorrect,
          "short implementation answers must also reject the preprocessing digraph");
}

void TestAdaptiveLevels() {
  {
    QuestionBank repeated_bank({MakeQuestion("same-family", "core", 3)});
    QuizSessionService repeated_service(repeated_bank);
    auto repeated_state = AdaptiveLevelState{.level = 3};
    for (int repeat = 0; repeat < 4; ++repeat) {
      const auto practice = repeated_service.Start(QuestionSelection{}, repeated_state, {}, {});
      repeated_state = repeated_service.SubmitAnswer(practice, "a").session.adaptive_state;
    }
    Require(repeated_state.level == 3, "repeating one family must never promote working difficulty");
  }
  auto state = AdaptiveLevelState{.level = 3};

  state = ApplyAdaptiveEvidence(state, 3, true, 1, "family.a", false);
  state = ApplyAdaptiveEvidence(state, 3, true, 1, "family.b", true);
  Require(state.level == 3 && state.promotion.families.size() == 2,
          "two clean families must not promote working difficulty");
  state = ApplyAdaptiveEvidence(state, 3, true, 1, "family.a", true);
  Require(state.level == 3 && state.promotion.families.size() == 2,
          "an application variant must not count as a new family");
  state = ApplyAdaptiveEvidence(state, 3, true, 1, "family.c", false);
  Require(state.level == 4 && state.promotion.families.empty() && state.evidence == 0,
          "three clean families including application must promote and clear evidence");
  state = ApplyAdaptiveEvidence(state, 3, true, 1, "family.d", true);
  Require(state.level == 4 && state.promotion.families.empty(), "an easy answer must not confirm a higher level");

  auto recognition = AdaptiveLevelState{.level = 3, .evidence = 2};
  for (const auto* family : {"recognition.a", "recognition.b", "recognition.c", "recognition.d"})
    recognition = ApplyAdaptiveEvidence(recognition, 3, true, 1, family, false);
  Require(recognition.level == 3 && recognition.promotion.families.size() == 4,
          "recognition-only families or old scalar evidence must not promote");
  recognition = ApplyAdaptiveEvidence(recognition, 3, true, 2, "recognition.a", true);
  Require(recognition.level == 3 && !recognition.promotion.families.front().application,
          "a repaired application must not certify an already clean recognition family");
  recognition = ApplyAdaptiveEvidence(recognition, 4, true, 1, "hard.application", true);
  Require(recognition.level == 3 && recognition.promotion.families.size() == 4,
          "a harder application must not supply current-level evidence");
  recognition = ApplyAdaptiveEvidence(recognition, 3, true, 1, "recognition.a", true);
  Require(recognition.level == 4, "a clean same-family application must supply the missing application evidence");

  state = ApplyAdaptiveEvidence(state, 4, true, 3);
  Require(state.level == 4 && state.evidence == -2,
          "one question corrected only after repeated errors must not lower the level immediately");

  state = ApplyAdaptiveEvidence(state, 4, true, 3);
  Require(state.level == 3 && state.evidence == 0,
          "two questions corrected only after repeated errors must lower the working difficulty");
  Require(state.promotion.families.empty(), "a demotion must discard previous-level family evidence");

  const auto hard_repair = ApplyAdaptiveEvidence(AdaptiveLevelState{.level = 3}, 4, true, 3);
  Require(hard_repair.level == 3 && hard_repair.evidence == 0,
          "struggling with a question above the current level must not lower it");

  const auto second_try = ApplyAdaptiveEvidence(AdaptiveLevelState{.level = 3}, 3, true, 2);
  Require(second_try.level == 3 && second_try.evidence == 0,
          "a corrected answer must not provide positive level evidence");

  const auto third_try = ApplyAdaptiveEvidence(AdaptiveLevelState{.level = 3}, 3, true, 3);
  Require(third_try.level == 3 && third_try.evidence == -2,
          "repeated errors at the working level must count as difficulty evidence after correction");

  bool rejected_incorrect_completion = false;
  try {
    static_cast<void>(ApplyAdaptiveEvidence(AdaptiveLevelState{.level = 3}, 3, false, 3));
  } catch (const std::invalid_argument&) {
    rejected_incorrect_completion = true;
  }
  Require(rejected_incorrect_completion, "an incorrect answer must never be treated as completed evidence");

  auto legacy = AdaptiveLevelState{.level = 3, .evidence = 2};
  legacy = ApplyAdaptiveEvidence(legacy, 3, true, 1);
  Require(legacy.level == 3 && legacy.promotion.families.empty(),
          "an answer without family provenance must fail closed despite legacy scalar evidence");
  auto maximum = AdaptiveLevelState{.level = 10};
  for (const auto* family : {"max.a", "max.b", "max.c"})
    maximum = ApplyAdaptiveEvidence(maximum, 10, true, 1, family, true);
  Require(maximum.level == 10 && maximum.promotion.families.empty(), "promotion exceeded maximum difficulty");
  auto minimum = AdaptiveLevelState{.level = 1};
  minimum = ApplyAdaptiveEvidence(minimum, 1, true, 3);
  minimum = ApplyAdaptiveEvidence(minimum, 1, true, 3);
  Require(minimum.level == 1 && minimum.evidence == 0, "demotion exceeded minimum difficulty");

  const auto rejects = [](const AdaptiveLevelState& invalid) {
    try {
      ValidateAdaptiveLevelState(invalid);
    } catch (const std::invalid_argument&) {
      return true;
    }
    return false;
  };
  Require(rejects({.level = 3, .promotion = {.level = 2, .families = {{"old.family", true}}}}),
          "previous-level families were reusable at another level");
  Require(rejects({.level = 3, .promotion = {.level = 3, .families = {{"", true}}}}),
          "an empty evidence family id was accepted");
  Require(rejects({.level = 3, .promotion = {.level = 3, .families = {{"duplicate", false}, {"duplicate", true}}}}),
          "duplicate evidence families were accepted");
  Require(rejects({.level = 3, .promotion = {.level = 3, .families = {{std::string(257, 'x'), true}}}}),
          "an oversized evidence family id was accepted");
  Require(rejects({.level = 3, .promotion = {.level = 3, .families = {{"bad family", true}}}}),
          "an invalid evidence family id was accepted");
  auto oversized = AdaptiveLevelState{.level = 3};
  oversized.promotion.level = 3;
  for (int index = 0; index < 1001; ++index)
    oversized.promotion.families.push_back({"family." + std::to_string(index), false});
  Require(rejects(oversized), "an unbounded evidence vector was accepted");
  for (const auto* invalid_json :
       {R"({"level":3,"families":false})", R"({"level":3,"families":[1]})",
        R"({"level":3,"families":[{"id":"family","application":"true"}]})",
        R"({"level":3,"families":[{"id":"family"}]})",
        R"({"level":3,"families":[{"id":"a","application":true},{"id":"a","application":false}]})"}) {
    bool rejected = false;
    try {
      static_cast<void>(ParseAdaptivePromotion(boost::json::parse(invalid_json).as_object(), 3));
    } catch (const std::invalid_argument&) {
      rejected = true;
    }
    Require(rejected, "malformed serialized promotion evidence was accepted");
  }
  auto partial = AdaptiveLevelState{.level = 3, .promotion = {.level = 3, .families = {{"partial.a", true}}}};
  partial = ApplyAdaptiveEvidence(partial, 3, true, 3, "repair.b", true);
  Require(partial.promotion.families.size() == 1, "a repaired family supplied clean promotion evidence");
  partial = ApplyAdaptiveEvidence(partial, 3, true, 3, "repair.c", true);
  Require(partial.level == 2 && partial.promotion.families.empty(),
          "demotion retained family evidence from the previous difficulty");

  QuestionBank bank({MakeQuestion("level-three", "core", 3)});
  QuizSessionService service(bank);

  const AdaptiveLevelState two_families{
      .level = 3, .promotion = {.level = 3, .families = {{"earlier.a", true}, {"earlier.b", false}}}};
  const auto session = service.Start(QuestionSelection{}, two_families, {{"core", two_families}});
  const auto correct = service.SubmitAnswer(session, "a");
  Require(correct.completed && correct.session.adaptive_state.level == 4,
          "accumulated family evidence must raise the adaptive level");
  Require(service.TopicLevel(correct.session, "core") == 4, "accumulated evidence must raise the topic level");

  const auto new_topic_session = service.Start(QuestionSelection{}, AdaptiveLevelState{.level = 3, .evidence = 2}, {});
  const auto incomplete = service.SubmitAnswer(new_topic_session, "b");
  const auto new_topic_state = service.TopicState(incomplete.session, "core");
  Require(!incomplete.completed && new_topic_state.level == 3 && new_topic_state.evidence == 0,
          "a new topic must not inherit overall evidence before question completion");
  const auto separate_topic = service.Start(QuestionSelection{}, two_families, {});
  const auto separate_correct = service.SubmitAnswer(separate_topic, "a");
  Require(separate_correct.session.adaptive_state.level == 4 &&
              service.TopicState(separate_correct.session, "core").level == 3 &&
              service.TopicState(separate_correct.session, "core").promotion.families.size() == 1,
          "an unassessed topic inherited families from other topics");
  const auto advanced_overall = service.Start(QuestionSelection{}, AdaptiveLevelState{.level = 7}, {});
  Require(service.TopicState(advanced_overall, "core").level == 3,
          "an unassessed topic inherited advanced overall difficulty");
  const auto advanced_correct = service.SubmitAnswer(advanced_overall, "a");
  Require(service.TopicState(advanced_correct.session, "core").level == 3 &&
              service.TopicState(advanced_correct.session, "core").promotion.families.size() == 1 &&
              advanced_correct.session.adaptive_state.level == 7 &&
              advanced_correct.session.adaptive_state.promotion.families.empty(),
          "new topic did not collect its own L3 evidence independently from overall L7");
  UserProgressSummary unassessed;
  unassessed.adaptive_level = unassessed.unlocked_level = 7;
  auto new_material = MakeQuestion("unassessed-hard", "containers", 4);
  new_material.skill_id = "containers.map_lookup";
  Require(!CanIntroduceTrainingQuestion(new_material, unassessed),
          "advanced overall level unlocked new material in an unassessed topic");
  new_material.level = 3;
  Require(CanIntroduceTrainingQuestion(new_material, unassessed), "unassessed topic lost its initial L3 material");
  unassessed.topics = {{.topic = "containers", .adaptive_level = 4}};
  new_material.level = 4;
  Require(CanIntroduceTrainingQuestion(new_material, unassessed),
          "persisted topic level was ignored after removing global fallback");
}

void TestSkillMastery() {
  const SkillEvidenceSummary recognition{
      .successful_family_count = 1,
      .clean_family_count = 1,
  };
  Require(AchievedSkillMastery(recognition) == SkillMasteryStatus::kEncountered,
          "one recognition family must only mark the skill as encountered");

  const SkillEvidenceSummary demonstrated{
      .successful_family_count = 2,
      .clean_family_count = 2,
      .application_family_count = 1,
      .transfer_family_count = 1,
  };
  const auto confirmation = ApplySkillMastery(SkillMasteryStatus::kEncountered, demonstrated,
                                              SkillMasteryEvent{.due_failure = false, .clean_application = true});
  Require(confirmation.current == SkillMasteryStatus::kDemonstrated,
          "a second independent family with clean application must confirm the skill");

  const auto repeated_family = ApplySkillMastery(confirmation.current, demonstrated,
                                                 SkillMasteryEvent{.due_failure = false, .clean_application = true});
  Require(repeated_family.current == SkillMasteryStatus::kDemonstrated,
          "repeating the same evidence totals must not advance mastery");

  const auto recovery = ApplySkillMastery(confirmation.current, demonstrated,
                                          SkillMasteryEvent{.due_failure = true, .clean_application = false});
  Require(recovery.current == SkillMasteryStatus::kNeedsRecovery &&
              AchievedSkillMastery(demonstrated) == SkillMasteryStatus::kDemonstrated,
          "a due failure must request recovery without erasing confirmed evidence");

  const auto restored = ApplySkillMastery(
      recovery.current, demonstrated,
      SkillMasteryEvent{.due_failure = false, .clean_application = true, .recovery_evidence_after_start = true});
  Require(restored.current == SkillMasteryStatus::kDemonstrated,
          "a clean application after recovery started must restore demonstrated status");

  const auto retained = ApplySkillMastery(
      restored.current, demonstrated, SkillMasteryEvent{.clean_application = true, .delayed_retention_evidence = true});
  Require(retained.current == SkillMasteryStatus::kRetained,
          "a clean application in a later retention window must retain the skill");

  const auto retained_recovery = ApplySkillMastery(retained.current, demonstrated,
                                                   SkillMasteryEvent{.due_failure = true, .clean_application = false});
  const auto stale_restore =
      ApplySkillMastery(retained_recovery.current, demonstrated, SkillMasteryEvent{.clean_application = true});
  Require(stale_restore.current == SkillMasteryStatus::kNeedsRecovery,
          "an application outside the recovery window must not recover a skill");
  const auto recovered_again =
      ApplySkillMastery(retained_recovery.current, demonstrated,
                        SkillMasteryEvent{.clean_application = true, .recovery_evidence_after_start = true});
  Require(recovered_again.current == SkillMasteryStatus::kDemonstrated,
          "recovery must return to demonstrated rather than reuse historical retained status");
  const auto retained_again =
      ApplySkillMastery(recovered_again.current, demonstrated,
                        SkillMasteryEvent{.clean_application = true, .delayed_retention_evidence = true});
  Require(retained_again.current == SkillMasteryStatus::kRetained,
          "a recovered skill must be retainable in a new delayed cycle");
}

void TestReviewSchedule() {
  constexpr std::array expected_days{1, 3, 7, 14, 30};
  for (int stage = 0; stage <= 5; ++stage) {
    const auto clean = PlanQuestionReview(stage, true, 1, false);
    const auto next = std::min(stage + 1, 5);
    Require(
        clean.stage == next && clean.delay == std::chrono::days{expected_days.at(static_cast<std::size_t>(next - 1))},
        "clean review did not follow the interval sequence");
    const auto failed = PlanQuestionReview(stage, false, 1, false);
    Require(failed.stage == std::min(stage, 1) && failed.delay == std::chrono::minutes{10},
            "a failed review retained a long stage");
    for (const bool assisted : {false, true}) {
      const auto corrected = PlanQuestionReview(stage, true, 2, assisted);
      Require(corrected.stage == std::min(stage, 1) && corrected.delay == std::chrono::days{1},
              "a corrected review advanced or retained a long interval");
    }
    const auto assisted = PlanQuestionReview(stage, true, 1, true);
    Require(assisted.stage == stage && assisted.delay == std::chrono::days{1},
            "help alone must not advance the review stage or count as an error");
  }
  const auto repaired = PlanQuestionReview(5, true, 2, false);
  const auto later_clean = PlanQuestionReview(repaired.stage, true, 1, false);
  Require(later_clean.stage == 2 && later_clean.delay == std::chrono::days{3},
          "one clean answer after repair jumped straight back to 30 days");
  for (const int invalid : {-1, 6}) {
    bool rejected = false;
    try {
      static_cast<void>(PlanQuestionReview(invalid, true, 1, false));
    } catch (const std::invalid_argument&) {
      rejected = true;
    }
    Require(rejected, "an invalid review stage was accepted");
  }
  bool rejected = false;
  try {
    static_cast<void>(PlanQuestionReview(0, true, 0, false));
  } catch (const std::invalid_argument&) {
    rejected = true;
  }
  Require(rejected, "zero attempts were accepted by the review policy");
}

void TestLevelProgressPolicy() {
  Require(!IsProgressionPractice(1, 5), "replaying level 1 after reaching level 5 must not count toward level 6");
  Require(!CanApplyLevelEvidence(IsProgressionPractice(1, 5), 1, 5),
          "answers from a past level must not change adaptive evidence");
  Require(IsProgressionPractice(5, 5), "the highest unlocked level must count as progression practice");
  Require(CanApplyLevelEvidence(IsProgressionPractice(5, 5), 5, 5),
          "answers at the highest unlocked level must be able to unlock the next level");
  Require(!CanApplyLevelEvidence(IsProgressionPractice(5, 5), 5, 6),
          "a fixed level 5 session must stop changing progress after level 6 is reached");
}

void TestReviewPriority() {
  QuestionBank bank({
      MakeQuestion("unseen-one", "core", 3),
      MakeQuestion("future", "core", 3),
      MakeQuestion("due", "core", 3),
      MakeQuestion("unseen-two", "core", 3),
  });
  QuizSessionService service(bank);

  std::unordered_map<std::string, QuestionReviewState> states{
      {"future", {.due = false, .stage = 2, .seconds_until_due = 3600}},
      {"due", {.due = true, .stage = 1, .failed_count = 2}},
  };

  const auto session = service.Start(QuestionSelection{}, 3, {}, states);
  Require(session.queue.Current() == "due", "due question must be selected before unseen and future questions");

  auto preferred_selection = QuestionSelection{};
  preferred_selection.preferred_question_ids = {"unseen-two"};
  const auto preferred = service.Start(preferred_selection, 3, {},
                                       {{"unseen-two", {.due = false, .stage = 2, .seconds_until_due = 3600}}});
  Require(preferred.queue.Current() == "unseen-two",
          "a recommended previously answered question must be placed before unseen questions");
}

void TestTrainingRecommendation() {
  auto due = MakeQuestion("due-raii", "language_core", 4);
  due.skill_id = "raii.fundamentals";
  auto recovery = MakeQuestion("recovery-virtual", "oop", 4);
  recovery.skill_id = "oop.virtual_functions";
  recovery.evidence_type = QuestionEvidenceType::kApplication;
  recovery.transfer = true;
  auto recognition = MakeQuestion("raii-recognition", "language_core", 3);
  recognition.skill_id = "raii.fundamentals";
  recognition.question_family_id = "raii.fundamentals.recognition";
  auto application = MakeQuestion("raii-application", "language_core", 3);
  application.skill_id = "raii.fundamentals";
  application.question_family_id = "raii.fundamentals.application";
  application.evidence_type = QuestionEvidenceType::kApplication;
  application.transfer = true;
  auto safe_new = MakeQuestion("storage-safe", "language_core", 2);
  safe_new.skill_id = "core.storage_duration";
  auto hard_new = MakeQuestion("storage-hard", "language_core", 5);
  hard_new.skill_id = "core.storage_duration";

  QuestionBank bank({due, recovery, recognition, application, safe_new, hard_new});
  UserProgressSummary summary{
      .adaptive_level = 4,
      .unlocked_level = 5,
      .topics = {{.topic = "language_core", .adaptive_level = 4}, {.topic = "oop", .adaptive_level = 4}},
      .skills = {{.skill_id = "oop.virtual_functions", .mastery_status = SkillMasteryStatus::kNeedsRecovery},
                 {.skill_id = "raii.fundamentals",
                  .mastery_status = SkillMasteryStatus::kEncountered,
                  .successful_family_count = 1,
                  .clean_family_count = 1}},
  };
  const std::unordered_set<std::string> clean_families{"raii.fundamentals.recognition"};

  const auto due_recommendation =
      RecommendTraining(bank, summary, {{"due-raii", {.due = true, .stage = 1, .failed_count = 1}}}, clean_families);
  Require(due_recommendation.reason == TrainingRecommendationReason::kDueReview &&
              due_recommendation.question_id == "due-raii",
          "a due review must take priority over recovery and missing evidence");

  const auto recovery_recommendation = RecommendTraining(bank, summary, {}, clean_families);
  Require(recovery_recommendation.reason == TrainingRecommendationReason::kRecovery &&
              recovery_recommendation.question_id == "recovery-virtual",
          "a skill that failed delayed review must be recovered before new material");

  summary.skills.erase(summary.skills.begin());
  const auto evidence_recommendation = RecommendTraining(bank, summary, {}, clean_families);
  Require(evidence_recommendation.reason == TrainingRecommendationReason::kMissingEvidence &&
              evidence_recommendation.question_id == "raii-application",
          "an encountered skill must prefer a clean application from a new family");

  auto new_recognition = recognition;
  new_recognition.id = "new-raii-recognition";
  new_recognition.question_family_id = "raii.fundamentals.new-recognition";
  auto seen_application = application;
  seen_application.id = "seen-raii-application";
  seen_application.question_family_id = "raii.fundamentals.seen-application";
  QuestionBank evidence_type_bank({new_recognition, seen_application});
  const auto required_application =
      RecommendTraining(evidence_type_bank, summary, {{"seen-raii-application", {.due = false, .stage = 1}}},
                        {"raii.fundamentals.seen-application"});
  Require(required_application.reason == TrainingRecommendationReason::kMissingEvidence &&
              required_application.question_id == "seen-raii-application",
          "required application evidence must outrank a newer recognition question");

  summary.skills.clear();
  summary.topics.clear();
  summary.adaptive_level = 3;
  const auto new_skill_recommendation = RecommendTraining(bank, summary, {}, {});
  Require(new_skill_recommendation.reason == TrainingRecommendationReason::kNewSkill &&
              new_skill_recommendation.question_id == "storage-safe" && new_skill_recommendation.level == 2,
          "new material must stay within the current difficulty even when a harder level is unlocked");
}

void TestTopicSelection() {
  QuestionBank bank({
      MakeQuestion("language", "language_core", 1),
      MakeQuestion("object", "oop", 1),
      MakeQuestion("template", "templates", 2),
  });

  const QuestionSelection selection{
      .topics = {"language_core", "oop"},
      .skill_ids = {},
      .kinds = {},
      .min_level = 1,
      .max_level = 10,
      .preferred_question_ids = {},
  };

  const auto selected = bank.Select(selection);
  Require(selected.size() == 2 && std::ranges::find(selected, "language") != selected.end() &&
              std::ranges::find(selected, "object") != selected.end(),
          "question selection must support multiple topics");

  const auto level_two = bank.Select(QuestionSelection{
      .topics = {},
      .skill_ids = {},
      .kinds = {},
      .matching_only = false,
      .min_level = 2,
      .max_level = 2,
      .preferred_question_ids = {},
  });
  Require(level_two == std::vector<std::string>{"template"},
          "an exact-level practice selection must not include easier questions");

  const std::vector<TrainingContentMode> modes{TrainingContentMode::kTheory};
  const std::vector<std::string> topics{"oop"};
  const auto mode_selection = QuestionSelectionForModes(modes, topics);
  Require(mode_selection.topics == topics && mode_selection.kinds == std::vector{QuestionKind::kTheory},
          "training modes and topics must form one question selection");

  bool duplicate_rejected = false;

  try {
    static_cast<void>(bank.Select(QuestionSelection{
        .topics = {"oop", "oop"},
        .skill_ids = {},
        .kinds = {},
        .min_level = 1,
        .max_level = 10,
        .preferred_question_ids = {},
    }));
  } catch (const std::invalid_argument&) {
    duplicate_rejected = true;
  }

  Require(duplicate_rejected, "duplicate selected topics must be rejected");
  Require(TrainingTopics().size() == 9 && TopicTitle("move_semantics") == "Семантика перемещения",
          "topic catalog must be the source of readable topic names");
}

void TestSkillCatalogAndSelection() {
  ValidateSkillCatalog();
  ValidateLearningModuleCatalog();
  Require(MasteryPilotSkillIds().size() == 20,
          "mastery pilot must keep a representative set of twenty independently checked skills");
  Require(IsMasteryPilotSkill("oop.virtual_functions") && !IsMasteryPilotSkill("core.const"),
          "only skills with a sufficient independent question bank must be marked assessable");

  const auto* lifetime_raii = FindLearningModule("lifetime_raii");
  Require(lifetime_raii != nullptr && lifetime_raii->skill_ids.size() == 4 &&
              lifetime_raii->skill_ids.front() == "core.storage_duration" &&
              lifetime_raii->skill_ids.back() == "lifetime.smart_pointers" &&
              lifetime_raii->minimum_question_families_per_skill == 4 &&
              lifetime_raii->minimum_transfer_questions_per_skill == 1,
          "lifetime/RAII module must expose its ordered skill path");

  const auto* virtual_destruction = FindSkill("oop.virtual_destruction");
  Require(virtual_destruction != nullptr && virtual_destruction->topic == "oop" &&
              std::ranges::find(virtual_destruction->prerequisites, "oop.virtual_functions") !=
                  virtual_destruction->prerequisites.end(),
          "skill catalog must expose topic and prerequisite relationships");

  const auto* static_polymorphism = FindSkill("oop.static_polymorphism");
  const auto* dynamic_polymorphism = FindSkill("oop.dynamic_polymorphism");
  Require(static_polymorphism != nullptr && dynamic_polymorphism != nullptr &&
              std::ranges::find(dynamic_polymorphism->prerequisites, "oop.inheritance") !=
                  dynamic_polymorphism->prerequisites.end(),
          "static and dynamic polymorphism must be separate skills with explicit prerequisites");

  auto object = MakeQuestion("object", "oop", 1);
  object.skill_id = "oop.objects";
  auto inheritance = MakeQuestion("inheritance", "oop", 2);
  inheritance.skill_id = "oop.inheritance";
  auto scope = MakeQuestion("scope", "language_core", 1);
  scope.skill_id = "core.scope";
  QuestionBank bank({object, inheritance, scope});

  const auto selected = bank.Select(QuestionSelection{
      .topics = {},
      .skill_ids = {"oop.inheritance"},
      .kinds = {},
      .preferred_question_ids = {},
  });
  Require(selected == std::vector<std::string>{"inheritance"},
          "question selection must filter by an explicit skill id");

  bool unknown_rejected = false;
  try {
    static_cast<void>(bank.Select(QuestionSelection{
        .topics = {},
        .skill_ids = {"missing.skill"},
        .kinds = {},
        .preferred_question_ids = {},
    }));
  } catch (const std::invalid_argument&) {
    unknown_rejected = true;
  }
  Require(unknown_rejected, "unknown selected skills must be rejected");
}

void TestMiniAppMenuRenderer() {
  const auto menu = boost::json::serialize(BuildMiniAppMenuMessage(42, "https://learn.example.com/"));
  Require(menu.contains("web_app") && menu.contains("https://learn.example.com/") && !menu.contains("callback_data") &&
              !menu.contains("training:start"),
          "Telegram menu must open the Mini App without exposing chat training callbacks");

  const auto unavailable = boost::json::serialize(BuildMiniAppMenuMessage(42, {}));
  Require(unavailable.contains("Mini App") && unavailable.contains("недоступен") && !unavailable.contains("web_app"),
          "Telegram menu must explain when the Mini App URL is not configured");
}

void TestProgressSummaryRenderer() {
  const UserProgressSummary summary{
      .adaptive_level = 4,
      .adaptive_evidence = 2,
      .sessions_started = 3,
      .sessions_completed = 2,
      .questions_seen = 7,
      .completed_count = 10,
      .correct_count = 8,
      .failed_count = 2,
      .first_try_correct_count = 6,
      .due_review_count = 2,
      .topics = {{.topic = "core_<templates>",
                  .adaptive_level = 5,
                  .adaptive_evidence = -1,
                  .completed_count = 10,
                  .correct_count = 8,
                  .failed_count = 2,
                  .first_try_correct_count = 6}},
      .skills = {},
      .answers = {.attempts = 15, .correct_attempts = 8, .first_answers = 12, .correct_first_answers = 6},
  };

  const auto message = BuildProgressSummaryMessage(42, summary);
  const auto& value = message.at("text").as_string();
  const std::string text(value.data(), value.size());

  Require(text.contains("Доля с первой попытки: 50% (6/12)"),
          "summary must include first answers on unfinished cards, not divide by completions");
  Require(text.contains("Верных попыток: 8/15") && text.contains("с первого раза среди завершённых"),
          "summary must distinguish attempts and legacy topic completion ratios");
  Require(text.contains("Core &lt;templates&gt;"), "summary topic must be escaped for Telegram HTML");
  Require(!text.contains("⬆️ 2/4") && text.contains("0/3") && text.contains("⬇️ 1/4"),
          "summary must not present old scalar evidence as promotion progress");
}

std::array<unsigned char, 32> TestHmac(std::string_view key, std::string_view value) {
  std::array<unsigned char, EVP_MAX_MD_SIZE> digest{};
  unsigned int size = 0;
  Require(key.size() <= static_cast<std::size_t>(INT_MAX), "test HMAC key is too large");
  Require(HMAC(EVP_sha256(), key.data(), static_cast<int>(key.size()),
               reinterpret_cast<const unsigned char*>(value.data()), value.size(), digest.data(), &size) != nullptr &&
              size == 32,
          "test HMAC failed");
  std::array<unsigned char, 32> result{};
  std::copy_n(digest.begin(), result.size(), result.begin());
  return result;
}

std::string Hex(std::span<const unsigned char> bytes) {
  constexpr std::string_view digits = "0123456789abcdef";
  std::string result;
  result.reserve(bytes.size() * 2);
  for (const auto byte : bytes) {
    result.push_back(digits[byte >> 4]);
    result.push_back(digits[byte & 0x0f]);
  }
  return result;
}

std::string PercentEncode(std::string_view input) {
  constexpr std::string_view digits = "0123456789ABCDEF";
  std::string result;
  for (const unsigned char value : input) {
    const bool safe = (value >= 'a' && value <= 'z') || (value >= 'A' && value <= 'Z') ||
                      (value >= '0' && value <= '9') || value == '-' || value == '_' || value == '.' || value == '~';
    if (safe) {
      result.push_back(static_cast<char>(value));
    } else {
      result.push_back('%');
      result.push_back(digits[value >> 4]);
      result.push_back(digits[value & 0x0f]);
    }
  }
  return result;
}

void TestTelegramInitDataValidation() {
  constexpr std::string_view token = "123456:test-token";
  constexpr std::int64_t timestamp = 1'700'000'000;
  const std::string user = R"({"id":42,"first_name":"Ada","username":"ada_cpp"})";
  const std::string check_string =
      "auth_date=" + std::to_string(timestamp) +
      "\nquery_id=query-1\nsignature=test-signature\nstart_param=review_212_v1\nuser=" + user;
  const auto secret = TestHmac("WebAppData", token);
  const auto hash =
      TestHmac(std::string_view(reinterpret_cast<const char*>(secret.data()), secret.size()), check_string);
  const std::string init_data =
      "auth_date=" + std::to_string(timestamp) +
      "&query_id=query-1&signature=test-signature&start_param=review_212_v1&user=" + PercentEncode(user) +
      "&hash=" + Hex(hash);
  const auto now = std::chrono::system_clock::time_point{std::chrono::seconds(timestamp + 30)};
  const auto valid = ValidateTelegramInitData(init_data, token, now);
  Require(valid && valid->user.id == 42 && valid->user.first_name == "Ada" && valid->query_id == "query-1" &&
              valid->start_param == "review_212_v1",
          "valid Telegram initData must authenticate the user");

  auto tampered = init_data;
  const auto name = tampered.find("Ada");
  Require(name != std::string::npos, "test initData must contain user name");
  tampered.replace(name, 3, "Eve");
  Require(!ValidateTelegramInitData(tampered, token, now), "tampered Telegram initData must be rejected");
  auto swapped_challenge = init_data;
  swapped_challenge.replace(swapped_challenge.find("review_212_v1"), 13, "review_213_v1");
  Require(!ValidateTelegramInitData(swapped_challenge, token, now), "a modified signed challenge must be rejected");

  const auto expired_now = std::chrono::system_clock::time_point{std::chrono::seconds(timestamp + 25 * 60 * 60)};
  Require(!ValidateTelegramInitData(init_data, token, expired_now), "expired Telegram initData must be rejected");

  const std::string invalid_date = std::to_string(std::numeric_limits<std::int64_t>::min());
  const std::string invalid_check_string = "auth_date=" + invalid_date + "\nuser=" + user;
  const auto invalid_hash =
      TestHmac(std::string_view(reinterpret_cast<const char*>(secret.data()), secret.size()), invalid_check_string);
  const std::string invalid_init_data =
      "auth_date=" + invalid_date + "&user=" + PercentEncode(user) + "&hash=" + Hex(invalid_hash);
  Require(!ValidateTelegramInitData(invalid_init_data, token, now),
          "non-positive Telegram auth_date must be rejected safely");
}

void TestSharedChallengeCatalog() {
  const QuestionBank bank(LoadQuestions("content/questions"));
  Require(kSharedChallenges.size() == 2, "the pilot must expose only its reviewed pair");
  for (const auto& definition : kSharedChallenges) {
    const auto& question = bank.Get(definition.question_id);
    Require(SharedChallengeForQuestion(question) == &definition && FindSharedChallenge(definition.id) == &definition,
            "a public challenge is not its pinned reviewed revision");
    auto changed = question;
    changed.content_revision = std::string(64, '0');
    Require(SharedChallengeForQuestion(changed) == nullptr, "editing a card must withdraw its public link");
    UserProgressSummary summary;
    Require(CanIntroduceTrainingQuestion(question, summary),
            "the map review pilot is not eligible at the initial level");
    summary.unlocked_level = 2;
    Require(!CanIntroduceTrainingQuestion(question, summary), "a link bypassed unlocked difficulty");
    summary.unlocked_level = 5;
    summary.topics.push_back({.topic = "containers", .adaptive_level = 2});
    Require(!CanIntroduceTrainingQuestion(question, summary), "a link bypassed the topic ceiling");
  }
  Require(!FindSharedChallenge("CR-000212") && !FindSharedChallenge("../../review_212_v1"),
          "arbitrary link selectors accepted");
  Require(IsValidBotUsername("cpp_recall_bot") && !IsValidBotUsername("@cpp_recall_bot") &&
              !IsValidBotUsername("https://t.me/cpp_recall_bot") && !IsValidBotUsername("x?startapp=evil"),
          "unsafe bot URL configuration accepted");
}

void TestBoundedExecutor() {
  BoundedExecutor parallel(2, 2);
  const auto initial_stats = parallel.Snapshot();
  Require(initial_stats.workers == 2 && initial_stats.active == 0 && initial_stats.pending == 0,
          "executor must report all created workers before any tasks run");
  std::promise<void> blocker_started;
  auto blocker_started_future = blocker_started.get_future();
  std::promise<void> release_blocker;
  auto release_blocker_future = release_blocker.get_future().share();
  std::promise<void> second_finished;
  auto second_finished_future = second_finished.get_future();

  Require(parallel.TrySubmit([&] {
    blocker_started.set_value();
    release_blocker_future.wait();
  }),
          "executor rejected its first task");
  blocker_started_future.wait();
  Require(parallel.TrySubmit([&] { second_finished.set_value(); }), "executor rejected a parallel task");
  Require(second_finished_future.wait_for(std::chrono::seconds(1)) == std::future_status::ready,
          "a blocked worker must not prevent another worker from serving a client");
  release_blocker.set_value();
  parallel.Stop();

  BoundedExecutor bounded(1, 1);
  std::promise<void> only_worker_started;
  auto only_worker_started_future = only_worker_started.get_future();
  std::promise<void> release_only_worker;
  auto release_only_worker_future = release_only_worker.get_future().share();
  Require(bounded.TrySubmit([&] {
    only_worker_started.set_value();
    release_only_worker_future.wait();
  }),
          "bounded executor rejected its active task");
  only_worker_started_future.wait();
  Require(bounded.TrySubmit([] {}), "bounded executor rejected a task within its queue limit");
  Require(!bounded.TrySubmit([] {}), "bounded executor accepted more pending tasks than configured");
  const auto stats = bounded.Snapshot();
  Require(stats.active == 1 && stats.pending == 1 && stats.pending_limit == 1 && stats.rejected == 1,
          "executor saturation and rejections were not observable");
  Require(stats.workers == 1, "executor must report its created worker while busy");
  release_only_worker.set_value();
  bounded.Stop();
  Require(bounded.Snapshot().active == 0 && bounded.Snapshot().pending == 0, "executor counters did not drain on stop");
  Require(bounded.Snapshot().workers == 0, "stopped executor must report no workers");
  bounded.Stop();
  const auto stopped_stats = bounded.Snapshot();
  Require(stopped_stats.workers == 0 && stopped_stats.active == 0 && stopped_stats.pending == 0 &&
              stopped_stats.pending_limit == 1 && stopped_stats.rejected == 1,
          "repeated stop must preserve the stopped executor statistics");
  Require(!bounded.TrySubmit([] {}), "stopped executor accepted a task");
  Require(bounded.Snapshot().rejected == 2, "rejection after stop was not counted");
  RuntimeMetrics metrics;
  metrics.RejectConnection();
  metrics.ReadFailure();
  metrics.WriteFailure();
  Require(metrics.Snapshot().at("rejected_connections").as_int64() == 1 &&
              metrics.Snapshot().at("http_read_errors").as_int64() == 1 &&
              metrics.Snapshot().at("http_write_errors").as_int64() == 1,
          "connection failures disappeared from HTTP metrics");
}

void TestKeyedMutex() {
  KeyedMutex keyed_mutex;
  std::promise<void> same_key_started;
  auto same_key_started_future = same_key_started.get_future();
  std::promise<void> same_key_acquired;
  auto same_key_acquired_future = same_key_acquired.get_future();
  std::future<void> same_key_task;

  {
    auto first_guard = keyed_mutex.Acquire(42);
    same_key_task = std::async(std::launch::async, [&] {
      same_key_started.set_value();
      auto guard = keyed_mutex.Acquire(42);
      same_key_acquired.set_value();
    });
    same_key_started_future.wait();
    Require(same_key_acquired_future.wait_for(std::chrono::milliseconds(50)) == std::future_status::timeout,
            "the same user key was allowed to run concurrently");

    auto different_key_task = std::async(std::launch::async, [&] { auto guard = keyed_mutex.Acquire(43); });
    Require(different_key_task.wait_for(std::chrono::seconds(1)) == std::future_status::ready,
            "one user key blocked an unrelated user key");
    different_key_task.get();
  }

  Require(same_key_acquired_future.wait_for(std::chrono::seconds(1)) == std::future_status::ready,
          "the next operation for a user did not resume after the previous operation finished");
  same_key_task.get();
}

}  // namespace

void CheckAnswerHelpContent();

int main() {
  try {
    TestLearningQueue();
    TestTheoryHelpSelection();
    TestMiniAppSessionSnapshot();
    CheckAnswerHelpContent();
    TestQuestionIdentity();
    CheckContentReviews();
    CheckReferenceBindings();
    TestAnswerChecking();
    TestBugHunterValidation();
    TestAdaptiveLevels();
    TestSkillMastery();
    TestReviewSchedule();
    TestLevelProgressPolicy();
    TestReviewPriority();
    TestTrainingRecommendation();
    CheckTrainingPlans();
    CheckRecoveryPriorityScenarios();
    CheckCppCurriculum();
    TestTopicSelection();
    TestSkillCatalogAndSelection();
    TestMiniAppMenuRenderer();
    TestProgressSummaryRenderer();
    TestTelegramInitDataValidation();
    TestSharedChallengeCatalog();
    TestBoundedExecutor();
    TestKeyedMutex();
  } catch (const std::exception& error) {
    std::cerr << "core_tests failed: " << error.what() << '\n';
    return 1;
  }

  std::cout << "core_tests passed\n";
  return 0;
}
