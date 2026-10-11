#include "app/quiz_session_service.h"
#include "bot/card_renderer.h"
#include "learning/learning_queue.h"
#include "learning/progress_summary.h"
#include "learning/review_state.h"
#include "quiz/question.h"
#include "quiz/question_bank.h"
#include "quiz/quiz_service.h"

#include <boost/json.hpp>

#include <iostream>
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
  QuestionBank bank({MakeQuestion("level-three", "core", 3)});
  QuizSessionService service(bank);

  const auto session = service.Start(QuestionSelection{}, 3);
  const auto correct = service.SubmitAnswer(session, "a");
  Require(correct.completed && correct.session.target_level == 4,
          "first-try answer at target level must raise adaptive level");
  Require(service.TopicLevel(correct.session, "core") == 4, "first-try answer must raise topic level");

  auto retry = service.SubmitAnswer(session, "b");
  retry = service.SubmitAnswer(retry.session, "b");
  retry = service.SubmitAnswer(retry.session, "b");
  Require(retry.completed && retry.session.target_level == 2, "three wrong answers must lower adaptive level");
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

void TestRendererEscapesHtml() {
  const auto question = MakeQuestion("render", "language_core", 1);
  const std::vector<std::string> order{"a", "b"};
  const TrainingProgressView progress{
      .topic = "language_core",
      .topic_level = 1,
      .adaptive_level = 1,
      .completed_questions = 0,
      .session_goal = 1,
      .question_number = 1,
  };

  const auto message = BuildQuestionMessage(1, question, order, {}, progress);
  const auto& value = message.at("text").as_string();
  const std::string text(value.data(), value.size());
  Require(text.contains("&lt;&amp;&gt;"), "question text must be escaped for Telegram HTML");
}

void TestProgressSummaryRenderer() {
  const UserProgressSummary summary{
      .adaptive_level = 4,
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
}

}  // namespace

int main() {
  try {
    TestLearningQueue();
    TestAnswerChecking();
    TestAdaptiveLevels();
    TestReviewPriority();
    TestRendererEscapesHtml();
    TestProgressSummaryRenderer();
  } catch (const std::exception& error) {
    std::cerr << "core_tests failed: " << error.what() << '\n';
    return 1;
  }

  std::cout << "core_tests passed\n";
  return 0;
}
