#include "storage/attempt_repository.h"

#include <chrono>
#include <cstdlib>
#include <exception>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>

namespace {

void Require(bool condition, std::string_view message) {
  if (!condition) {
    throw std::runtime_error(std::string(message));
  }
}

}  // namespace

int main() {
  const char* database_url = std::getenv("TEST_DATABASE_URL");
  if (database_url == nullptr || *database_url == '\0') {
    std::cout << "storage_tests skipped: TEST_DATABASE_URL is not set\n";
    return 77;
  }

  try {
    AttemptRepository repository(database_url);
    const auto timestamp = std::chrono::steady_clock::now().time_since_epoch().count();
    const auto user_id = -1 - static_cast<std::int64_t>(timestamp % 1'000'000'000);
    const auto session_id = repository.StartSession(user_id, "integration-test");

    const SubmissionRecord submission{
        .user_id = user_id,
        .session_id = session_id,
        .question_id = "integration-question",
        .topic = "integration-topic",
        .answer = "a",
        .correct = true,
        .completed = true,
        .attempt_number = 1,
        .topic_level = 4,
        .adaptive_level = 4,
    };

    Require(repository.RecordSubmission(submission) == RecordSubmissionResult::kInserted,
            "first submission must be inserted");
    Require(repository.RecordSubmission(submission) == RecordSubmissionResult::kDuplicate,
            "repeated submission must be idempotent");

    const auto states = repository.LoadReviewStates(user_id);
    const auto state = states.find("integration-question");
    Require(state != states.end(), "question progress was not stored");
    Require(state->second.stage == 1 && state->second.correct_count == 1 && state->second.failed_count == 0,
            "duplicate submission changed question progress");

    Require(repository.GetAdaptiveLevel(user_id) == 4, "adaptive level was not stored");
    Require(repository.LoadTopicLevels(user_id).at("integration-topic") == 4, "topic level was not stored");

    const auto summary = repository.LoadProgressSummary(user_id);
    Require(summary.adaptive_level == 4, "summary adaptive level is incorrect");
    Require(summary.sessions_started == 1 && summary.sessions_completed == 0,
            "summary session counts are incorrect before completion");
    Require(summary.questions_seen == 1 && summary.completed_count == 1 && summary.correct_count == 1 &&
                summary.failed_count == 0 && summary.first_try_correct_count == 1,
            "summary answer counts are incorrect");
    Require(summary.due_review_count == 0, "newly mastered question must not be due immediately");
    Require(summary.topics.size() == 1 && summary.topics.front().topic == "integration-topic" &&
                summary.topics.front().adaptive_level == 4 && summary.topics.front().completed_count == 1 &&
                summary.topics.front().correct_count == 1,
            "summary topic progress is incorrect");

    repository.FinishSession(session_id, "completed");

    Require(repository.LoadProgressSummary(user_id).sessions_completed == 1,
            "completed session was not reflected in summary");

    auto after_finish = submission;
    after_finish.attempt_number = 2;
    bool rejected = false;
    try {
      static_cast<void>(repository.RecordSubmission(after_finish));
    } catch (const std::runtime_error&) {
      rejected = true;
    }
    Require(rejected, "finished session accepted a new answer");
  } catch (const std::exception& error) {
    std::cerr << "storage_tests failed: " << error.what() << '\n';
    return 1;
  }

  std::cout << "storage_tests passed\n";
  return 0;
}
