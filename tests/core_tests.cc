#include "app/quiz_session_service.h"
#include "app/training_mode.h"
#include "bot/card_renderer.h"
#include "learning/adaptive_level.h"
#include "learning/learning_queue.h"
#include "learning/progress_summary.h"
#include "learning/review_state.h"
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
}

void TestTopicSelection() {
  QuestionBank bank({
      MakeQuestion("language", "language_core", 1),
      MakeQuestion("object", "oop", 1),
      MakeQuestion("template", "templates", 1),
  });

  const QuestionSelection selection{
      .topics = {"language_core", "oop"},
      .kinds = {},
      .min_level = 1,
      .max_level = 10,
  };

  const auto selected = bank.Select(selection);
  Require(selected.size() == 2 && std::ranges::find(selected, "language") != selected.end() &&
              std::ranges::find(selected, "object") != selected.end(),
          "question selection must support multiple topics");

  const std::vector<TrainingContentMode> modes{TrainingContentMode::kTheory};
  const std::vector<std::string> topics{"oop"};
  const auto mode_selection = QuestionSelectionForModes(modes, topics);
  Require(mode_selection.topics == topics && mode_selection.kinds == std::vector{QuestionKind::kTheory},
          "training modes and topics must form one question selection");

  bool duplicate_rejected = false;

  try {
    static_cast<void>(bank.Select(QuestionSelection{
        .topics = {"oop", "oop"},
        .kinds = {},
        .min_level = 1,
        .max_level = 10,
    }));
  } catch (const std::invalid_argument&) {
    duplicate_rejected = true;
  }

  Require(duplicate_rejected, "duplicate selected topics must be rejected");
  Require(TrainingTopics().size() == 9 && TopicTitle("move_semantics") == "Семантика перемещения",
          "topic catalog must be the source of readable topic names");
}

void TestTopicMenuRenderer() {
  const std::vector<TrainingContentMode> modes{TrainingContentMode::kTheory};
  const std::vector<std::string> topics{"oop", "templates"};

  const auto start_menu = boost::json::serialize(BuildStartMenuMessage(42, modes, topics));
  Require(start_menu.contains("topics:open") && start_menu.contains("Темы: 2"),
          "start menu must show the topic selector and selected count");

  const auto topic_menu = boost::json::serialize(BuildTopicMenuMessage(42, topics));
  Require(topic_menu.contains("topic:oop") && topic_menu.contains("✓ ООП") && topic_menu.contains("topics:done"),
          "topic menu must show selected topics and a return action");

  const std::vector<std::string> all_topics;
  const auto all_menu = boost::json::serialize(BuildTopicMenuMessage(42, all_topics));
  Require(all_menu.contains("✓ Все темы"), "empty topic selection must visibly mean all topics");
}

void TestRendererEscapesHtml() {
  const auto question = MakeQuestion("render", "language_core", 1);
  const std::vector<std::string> order{"a", "b"};
  const TrainingProgressView progress{
      .topic = "language_core",
      .topic_level = 1,
      .adaptive_level = 1,
      .question_level = 1,
      .completed_questions = 0,
      .session_goal = 1,
      .question_number = 1,
  };

  const auto message = BuildQuestionMessage(1, question, order, {}, progress);
  const auto& value = message.at("text").as_string();
  const std::string text(value.data(), value.size());
  Require(text.contains("&lt;&amp;&gt;"), "question text must be escaped for Telegram HTML");
}

void TestAnswerFeedbackRenderer() {
  const auto question = MakeQuestion("feedback", "move_semantics", 3);
  const std::vector<std::string> order{"a", "b"};
  const TrainingProgressView progress{
      .topic = "move_semantics",
      .topic_level = 4,
      .adaptive_level = 4,
      .question_level = 3,
      .completed_questions = 1,
      .session_goal = 3,
      .question_number = 1,
      .correct_attempts = 1,
      .total_attempts = 1,
      .current_streak = 1,
      .repeat = false,
      .attempt_number = 1,
  };

  const auto text = BuildAnswerText(question, order, "A",
                                    AnswerFeedbackView{
                                        .result = AnswerResult::kCorrect,
                                        .attempts = 1,
                                        .completed = true,
                                        .show_explanation = true,
                                        .previous_topic_level = 3,
                                        .previous_adaptive_level = 3,
                                    },
                                    progress);

  Require(text.contains("Верно с первой попытки"), "feedback must distinguish a first-try answer");
  Require(text.contains("Уровень темы: 3 → 4"), "feedback must show a topic level change");
  Require(text.contains("Семантика перемещения"), "known topics must have readable display names");

  const auto retry_text = BuildAnswerText(question, order, "B",
                                          AnswerFeedbackView{
                                              .result = AnswerResult::kIncorrect,
                                              .attempts = 1,
                                              .completed = false,
                                              .show_explanation = false,
                                              .previous_topic_level = 3,
                                              .previous_adaptive_level = 3,
                                          },
                                          TrainingProgressView{
                                              .topic = "move_semantics",
                                              .topic_level = 3,
                                              .adaptive_level = 3,
                                              .question_level = 3,
                                              .completed_questions = 0,
                                              .session_goal = 3,
                                              .question_number = 1,
                                              .correct_attempts = 0,
                                              .total_attempts = 1,
                                              .current_streak = 0,
                                              .repeat = false,
                                              .attempt_number = 1,
                                          });
  Require(retry_text.contains("осталось попыток: 2"), "feedback must explain the retry policy");
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
    TestReviewPriority();
    TestTopicSelection();
    TestTopicMenuRenderer();
    TestRendererEscapesHtml();
    TestAnswerFeedbackRenderer();
    TestProgressSummaryRenderer();
    TestTelegramInitDataValidation();
  } catch (const std::exception& error) {
    std::cerr << "core_tests failed: " << error.what() << '\n';
    return 1;
  }

  std::cout << "core_tests passed\n";
  return 0;
}
