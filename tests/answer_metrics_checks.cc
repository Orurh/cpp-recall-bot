#include "miniapp/miniapp_service.h"
#include "storage/attempt_repository.h"
#include "storage/question_loader.h"

#include <boost/json.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <stdexcept>
#include <string>

namespace {

void Check(bool condition, const char* message) {
  if (!condition)
    throw std::runtime_error(message);
}

TelegramMiniAppUser User(std::int64_t id) {
  return {.id = id, .first_name = {}, .last_name = {}, .username = {}, .language_code = {}};
}

}  // namespace

void CheckAnswerMetrics(const char* url) {
  auto questions = LoadQuestions("content/questions");
  const auto chosen = std::ranges::find_if(questions, [](const Question& q) {
    return q.skill_id == "core.storage_duration" && q.level == 1 && std::holds_alternative<SingleChoiceTask>(q.task);
  });
  Check(chosen != questions.end(), "answer metrics fixture unavailable");
  QuestionBank bank({*chosen});
  auto pool = std::make_shared<DatabaseConnectionPool>(url, 3);
  MiniAppService service(bank, pool);
  AttemptRepository repository(pool);
  const auto uid = -1000 - std::chrono::steady_clock::now().time_since_epoch().count() % 1'000'000'000;
  const auto summary = [&] {
    return service.Bootstrap(User(uid)).at("summary").as_object();
  };
  auto data = summary();
  Check(data.at("attempt_accuracy").is_null() && data.at("accuracy").is_null() &&
            data.at("first_answer_accuracy").is_null() && data.at("completion_rate").is_null(),
        "empty metrics were presented as a measured percentage");

  const auto session = service.StartSession(uid, {{"recommended", true}});
  Check(session.at("training_plan").as_object().at("new_shortage").as_bool() &&
            session.at("training_plan").as_object().at("total").as_uint64() == 1,
        "the sparse bank was hidden or padded instead of reporting a short training");
  const auto id = std::string(session.at("question").as_object().at("id").as_string());
  const auto& task = std::get<SingleChoiceTask>(bank.Get(id).task);
  const auto wrong =
      std::ranges::find_if(task.options, [&](const auto& option) { return option.id != task.correct_option_id; });
  const boost::json::object mistake{{"question_id", id}, {"answer", wrong->id}};
  static_cast<void>(service.SubmitAnswer(uid, mistake));
  static_cast<void>(service.SubmitAnswer(uid, mistake));  // Lost response retry, not another attempt.
  data = summary();
  Check(data.at("attempt_count").as_int64() == 1 && data.at("first_answer_count").as_int64() == 1 &&
            data.at("first_answer_accuracy").as_double() == 0.0 && data.at("completed").as_int64() == 0,
        "an unfinished mistake was excluded or a retry counted twice");
  Check(data.at("answer_statistics_by_topic").as_array().front().as_object().at("attempt_count").as_int64() == 1,
        "topic statistics excluded unfinished cards");
  static_cast<void>(service.OpenTheory(uid, {{"question_id", id}}));
  static_cast<void>(service.NextQuestion(uid));
  static_cast<void>(service.SubmitAnswer(uid, mistake));
  static_cast<void>(service.NextQuestion(uid));
  static_cast<void>(service.SubmitAnswer(uid, {{"question_id", id}, {"answer", task.correct_option_id}}));
  data = summary();
  Check(data.at("attempt_count").as_int64() == 3 && data.at("correct_attempt_count").as_int64() == 1 &&
            std::abs(data.at("attempt_accuracy").as_double() - 33.3) < 0.001 &&
            data.at("accuracy") == data.at("attempt_accuracy") && data.at("answer_metrics_version").as_int64() == 2,
        "wrong-wrong-correct was reported as 100% accuracy");
  Check(data.at("first_answer_accuracy").as_double() == 0.0 && data.at("completed").as_int64() == 1 &&
            data.at("sessions_completed").as_int64() == 0,
        "an assisted repair was promoted to first-answer success or full training completion");
  Check(data.at("answer_statistics_by_skill").as_array().front().as_object().at("attempt_accuracy") ==
            data.at("attempt_accuracy"),
        "skill and total metrics disagree");
  const auto complete = service.NextQuestion(uid);
  Check(complete.at("summary").as_object().at("completion_rate").as_double() == 100.0,
        "finishing all cards did not complete the training");

  // A stopped card must stay in the denominator of first answers.
  static_cast<void>(service.StartSession(uid, {{"recommended", true}}));
  static_cast<void>(service.SubmitAnswer(uid, mistake));
  const auto stopped = service.StopSession(uid).at("summary").as_object();
  Check(stopped.at("first_answer_count").as_int64() == 2 && stopped.at("first_answer_accuracy").as_double() == 0.0 &&
            stopped.at("attempt_count").as_int64() == 4 && stopped.at("completion_rate").as_double() == 50.0,
        "stopping a training erased an unsuccessful first answer");

  // Legacy attempts without an analytics event/revision still count. Removed cards stay unclassified.
  const auto legacy_id = repository.StartSession(uid, "legacy-chat");
  const SubmissionRecord legacy{.user_id = uid,
                                .session_id = legacy_id,
                                .question_id = "removed-legacy-card",
                                .topic = "legacy",
                                .skill_id = "legacy.skill",
                                .question_family_id = "legacy.family",
                                .answer = "a",
                                .correct = true,
                                .completed = true,
                                .attempt_number = 1};
  static_cast<void>(repository.RecordSubmission(legacy));
  static_cast<void>(repository.RecordSubmission(legacy));
  data = summary();
  Check(data.at("attempt_count").as_int64() == 5 && data.at("first_answer_count").as_int64() == 3 &&
            data.at("unclassified_attempt_count").as_int64() == 1,
        "legacy attempts were lost, duplicated or assigned a fictional current skill");

  auto assisted = legacy;
  assisted.question_id = id;
  assisted.assisted = true;
  static_cast<void>(repository.RecordSubmission(assisted));
  data = summary();
  Check(data.at("first_answer_count").as_int64() == 4 && data.at("correct_first_answer_count").as_int64() == 1 &&
            data.at("first_answer_accuracy").as_double() == 25.0,
        "an assisted first answer was reported as clean knowledge");

  const auto matching = std::ranges::find_if(
      questions, [](const Question& q) { return q.level <= 3 && std::holds_alternative<MatchingTask>(q.task); });
  Check(matching != questions.end(), "matching metrics fixture unavailable");
  QuestionBank matching_bank({*matching});
  MiniAppService matching_service(matching_bank, pool);
  const auto matching_uid = uid - 1'000'000'000;
  auto current = matching_service.StartSession(matching_uid, {{"recommended", true}});
  const auto& pairs = std::get<MatchingTask>(matching->task).pairs;
  const auto pick = [&](std::string_view side, std::string_view text) {
    const auto& board = current.at("question").as_object().at("task").as_object().at("board").as_object();
    const auto& column = board.at(side).as_array();
    const auto item = std::ranges::find_if(
        column, [&](const auto& value) { return value.as_object().at("text").as_string() == text; });
    Check(item != column.end(), "matching test pair was not rendered");
    auto response = matching_service.SelectMatching(matching_uid, {{"question_id", matching->id},
                                                                   {"side", side},
                                                                   {"position", item->as_object().at("position")},
                                                                   {"revision", board.at("revision")}});
    current = response.at("session").as_object();
  };
  for (const auto& pair : pairs) {
    pick("left", pair.left);
    const auto partial = matching_service.Bootstrap(User(matching_uid)).at("summary").as_object();
    Check(partial.at("attempt_count").as_int64() == 0, "a matching click was counted as an answer");
    pick("right", pair.right);
  }
  const auto matched = matching_service.Bootstrap(User(matching_uid)).at("summary").as_object();
  Check(matched.at("attempt_count").as_int64() == 1 && matched.at("first_answer_count").as_int64() == 1 &&
            matched.at("first_answer_accuracy").as_double() == 100.0,
        "a completed matching round was not counted as exactly one first answer");
}
