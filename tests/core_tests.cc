#include "app/quiz_session_service.h"
#include "app/training_mode.h"
#include "bot/card_renderer.h"
#include "learning/adaptive_level.h"
#include "learning/learning_queue.h"
#include "learning/level_progress_policy.h"
#include "learning/module_catalog.h"
#include "learning/progress_summary.h"
#include "learning/review_state.h"
#include "learning/skill_catalog.h"
#include "learning/skill_mastery.h"
#include "learning/training_recommendation.h"
#include "miniapp/telegram_init_data.h"
#include "quiz/question.h"
#include "quiz/question_bank.h"
#include "quiz/quiz_service.h"
#include "quiz/topic_catalog.h"

#include <boost/json.hpp>

#include <openssl/evp.h>
#include <openssl/hmac.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <climits>
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

namespace {

void Require(bool condition, std::string_view message) {
  if (!condition) {
    throw std::runtime_error(std::string(message));
  }
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

  static_cast<void>(queue.SubmitAnswer(true));
  Require(queue.Current() == "first", "requeued question must return");
  static_cast<void>(queue.SubmitAnswer(false));
  const auto last = queue.SubmitAnswer(false);
  Require(last.attempts == 3 && last.completed, "third wrong answer must complete question");
  Require(queue.Empty(), "queue must be empty after completed questions");
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
          "implementation answer must ignore whitespace");
}

void TestAdaptiveLevels() {
  auto state = AdaptiveLevelState{.level = 3};

  state = ApplyAdaptiveEvidence(state, 3, true, 1);
  Require(state.level == 3 && state.evidence == 2, "one correct answer must not raise the level immediately");

  state = ApplyAdaptiveEvidence(state, 3, true, 1);
  Require(state.level == 4 && state.evidence == 0, "two first-try answers must raise the level");

  state = ApplyAdaptiveEvidence(state, 3, true, 1);
  Require(state.level == 4 && state.evidence == 0, "an easy answer must not confirm a higher level");

  state = ApplyAdaptiveEvidence(state, 4, false, 3);
  Require(state.level == 4 && state.evidence == -2, "one failed question must not lower the level immediately");

  state = ApplyAdaptiveEvidence(state, 4, false, 3);
  Require(state.level == 3 && state.evidence == 0, "two failed questions must lower the level");

  const auto hard_failure = ApplyAdaptiveEvidence(AdaptiveLevelState{.level = 3}, 4, false, 3);
  Require(hard_failure.level == 3 && hard_failure.evidence == 0,
          "a failed question above the current level must not lower it");

  const auto second_try = ApplyAdaptiveEvidence(AdaptiveLevelState{.level = 3}, 3, true, 2);
  Require(second_try.level == 3 && second_try.evidence == 1,
          "a second-try answer must provide weaker positive evidence");

  const auto third_try = ApplyAdaptiveEvidence(AdaptiveLevelState{.level = 3}, 3, true, 3);
  Require(third_try.level == 3 && third_try.evidence == 0,
          "a third-try answer must complete the question without raising the level");

  QuestionBank bank({MakeQuestion("level-three", "core", 3)});
  QuizSessionService service(bank);

  const auto session = service.Start(QuestionSelection{}, AdaptiveLevelState{.level = 3, .evidence = 2},
                                     {{"core", AdaptiveLevelState{.level = 3, .evidence = 2}}});
  const auto correct = service.SubmitAnswer(session, "a");
  Require(correct.completed && correct.session.adaptive_state.level == 4,
          "accumulated evidence must raise the adaptive level");
  Require(service.TopicLevel(correct.session, "core") == 4, "accumulated evidence must raise the topic level");

  const auto new_topic_session = service.Start(QuestionSelection{}, AdaptiveLevelState{.level = 3, .evidence = 2}, {});
  const auto incomplete = service.SubmitAnswer(new_topic_session, "b");
  const auto new_topic_state = service.TopicState(incomplete.session, "core");
  Require(!incomplete.completed && new_topic_state.level == 3 && new_topic_state.evidence == 0,
          "a new topic must not inherit overall evidence before question completion");
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

  const auto restored = ApplySkillMastery(recovery.current, demonstrated,
                                          SkillMasteryEvent{.due_failure = false, .clean_application = true});
  Require(restored.current == SkillMasteryStatus::kDemonstrated,
          "a clean application must restore a skill without rebuilding its history");

  auto retained = demonstrated;
  retained.delayed_application_count = 1;
  Require(AchievedSkillMastery(retained) == SkillMasteryStatus::kRetained,
          "a delayed clean application on a new question must retain the skill");
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
  const auto preferred = service.Start(preferred_selection, 3, {}, {});
  Require(preferred.queue.Current() == "unseen-two",
          "a recommended unseen question must be placed first when no review is due");
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
  };

  const auto message = BuildProgressSummaryMessage(42, summary);
  const auto& value = message.at("text").as_string();
  const std::string text(value.data(), value.size());

  Require(text.contains("Точность: 80%"), "summary must calculate completion accuracy");
  Require(text.contains("Core &lt;templates&gt;"), "summary topic must be escaped for Telegram HTML");
  Require(text.contains("⬆️ 2/4") && text.contains("⬇️ 1/4"),
          "summary must visualize positive and negative level evidence");
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
      "auth_date=" + std::to_string(timestamp) + "\nquery_id=query-1\nsignature=test-signature\nuser=" + user;
  const auto secret = TestHmac("WebAppData", token);
  const auto hash =
      TestHmac(std::string_view(reinterpret_cast<const char*>(secret.data()), secret.size()), check_string);
  const std::string init_data = "auth_date=" + std::to_string(timestamp) +
                                "&query_id=query-1&signature=test-signature&user=" + PercentEncode(user) +
                                "&hash=" + Hex(hash);
  const auto now = std::chrono::system_clock::time_point{std::chrono::seconds(timestamp + 30)};
  const auto valid = ValidateTelegramInitData(init_data, token, now);
  Require(valid && valid->user.id == 42 && valid->user.first_name == "Ada" && valid->query_id == "query-1",
          "valid Telegram initData must authenticate the user");

  auto tampered = init_data;
  const auto name = tampered.find("Ada");
  Require(name != std::string::npos, "test initData must contain user name");
  tampered.replace(name, 3, "Eve");
  Require(!ValidateTelegramInitData(tampered, token, now), "tampered Telegram initData must be rejected");

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

}  // namespace

int main() {
  try {
    TestLearningQueue();
    TestAnswerChecking();
    TestAdaptiveLevels();
    TestSkillMastery();
    TestLevelProgressPolicy();
    TestReviewPriority();
    TestTrainingRecommendation();
    TestTopicSelection();
    TestSkillCatalogAndSelection();
    TestMiniAppMenuRenderer();
    TestProgressSummaryRenderer();
    TestTelegramInitDataValidation();
  } catch (const std::exception& error) {
    std::cerr << "core_tests failed: " << error.what() << '\n';
    return 1;
  }

  std::cout << "core_tests passed\n";
  return 0;
}
