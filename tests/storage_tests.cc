#include "storage/attempt_repository.h"

#include <libpq-fe.h>

#include <algorithm>
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

class SubmissionFailureTrigger {
 public:
  explicit SubmissionFailureTrigger(const char* database_url) : connection_(PQconnectdb(database_url)) {
    if (connection_ == nullptr || PQstatus(connection_) != CONNECTION_OK) {
      const std::string error = connection_ == nullptr ? "connection allocation failed" : PQerrorMessage(connection_);
      if (connection_ != nullptr) {
        PQfinish(connection_);
        connection_ = nullptr;
      }
      throw std::runtime_error("failed to prepare transaction failure test: " + error);
    }

    Execute(R"sql(
DROP TRIGGER IF EXISTS cpp_recall_test_fail_submission ON question_progress;
CREATE OR REPLACE FUNCTION cpp_recall_test_fail_submission()
RETURNS trigger
LANGUAGE plpgsql
AS $function$
BEGIN
    IF NEW.question_id LIKE 'integration-atomic-failure-%' THEN
        RAISE EXCEPTION 'forced submission failure';
    END IF;
    RETURN NEW;
END;
$function$;
CREATE TRIGGER cpp_recall_test_fail_submission
BEFORE INSERT OR UPDATE ON question_progress
FOR EACH ROW
EXECUTE FUNCTION cpp_recall_test_fail_submission();
)sql");
  }

  ~SubmissionFailureTrigger() {
    if (connection_ == nullptr) {
      return;
    }

    PGresult* result = PQexec(connection_, R"sql(
DROP TRIGGER IF EXISTS cpp_recall_test_fail_submission ON question_progress;
DROP FUNCTION IF EXISTS cpp_recall_test_fail_submission();
)sql");
    if (result != nullptr) {
      PQclear(result);
    }
    PQfinish(connection_);
  }

  SubmissionFailureTrigger(const SubmissionFailureTrigger&) = delete;
  SubmissionFailureTrigger& operator=(const SubmissionFailureTrigger&) = delete;

 private:
  void Execute(const char* sql) {
    PGresult* raw_result = PQexec(connection_, sql);
    if (raw_result == nullptr) {
      throw std::runtime_error("PostgreSQL returned no result while preparing transaction failure test");
    }

    const auto status = PQresultStatus(raw_result);
    const std::string error = status == PGRES_COMMAND_OK ? std::string{} : PQerrorMessage(connection_);
    PQclear(raw_result);
    if (status != PGRES_COMMAND_OK) {
      throw std::runtime_error("failed to prepare transaction failure test: " + error);
    }
  }

  PGconn* connection_ = nullptr;
};

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
        .skill_id = "integration.skill",
        .question_family_id = "integration.skill.recognition",
        .evidence_type = QuestionEvidenceType::kRecognition,
        .transfer = false,
        .answer = "a",
        .correct = true,
        .completed = true,
        .attempt_number = 1,
        .topic_level = 4,
        .topic_level_evidence = 2,
        .adaptive_level = 4,
        .adaptive_level_evidence = 2,
    };

    const auto first_outcome = repository.RecordSubmission(submission);
    Require(first_outcome.result == RecordSubmissionResult::kInserted && first_outcome.xp_gained == 12,
            "a new family solved on the first attempt must award 12 XP");
    const auto duplicate_outcome = repository.RecordSubmission(submission);
    Require(duplicate_outcome.result == RecordSubmissionResult::kDuplicate && duplicate_outcome.xp_gained == 0,
            "repeated submission must be idempotent and award no XP");

    const auto states = repository.LoadReviewStates(user_id);
    const auto state = states.find("integration-question");
    Require(state != states.end(), "question progress was not stored");
    Require(state->second.stage == 1 && state->second.correct_count == 1 && state->second.failed_count == 0,
            "duplicate submission changed question progress");
    Require(repository.LoadCleanQuestionFamilies(user_id).contains("integration.skill.recognition"),
            "a clean family must be available to the recommendation engine");

    Require(repository.GetAdaptiveLevel(user_id) == 4, "adaptive level was not stored");
    Require(repository.GetUnlockedLevel(user_id) == 4, "reached level was not unlocked");
    Require(repository.LoadTopicLevels(user_id).at("integration-topic") == 4, "topic level was not stored");
    Require(repository.GetAdaptiveState(user_id).evidence == 2, "adaptive evidence was not stored");
    Require(repository.LoadTopicStates(user_id).at("integration-topic").evidence == 2, "topic evidence was not stored");

    const auto summary = repository.LoadProgressSummary(user_id);
    Require(summary.adaptive_level == 4, "summary adaptive level is incorrect");
    Require(summary.unlocked_level == 4, "summary unlocked level is incorrect");
    Require(summary.adaptive_evidence == 2, "summary adaptive evidence is incorrect");
    Require(summary.sessions_started == 1 && summary.sessions_completed == 0,
            "summary session counts are incorrect before completion");
    Require(summary.questions_seen == 1 && summary.completed_count == 1 && summary.correct_count == 1 &&
                summary.failed_count == 0 && summary.first_try_correct_count == 1,
            "summary answer counts are incorrect");
    Require(summary.due_review_count == 0, "newly mastered question must not be due immediately");
    Require(summary.xp_total == 12, "summary XP total is incorrect after the first family");
    Require(summary.topics.size() == 1 && summary.topics.front().topic == "integration-topic" &&
                summary.topics.front().adaptive_level == 4 && summary.topics.front().completed_count == 1 &&
                summary.topics.front().correct_count == 1 && summary.topics.front().adaptive_evidence == 2,
            "summary topic progress is incorrect");
    Require(summary.skills.size() == 1 && summary.skills.front().skill_id == "integration.skill" &&
                summary.skills.front().mastery_status == SkillMasteryStatus::kEncountered &&
                summary.skills.front().clean_family_count == 1 && summary.skills.front().questions_seen == 1 &&
                summary.skills.front().completed_count == 1 && summary.skills.front().correct_count == 1 &&
                summary.skills.front().first_try_correct_count == 1,
            "summary skill progress is incorrect");

    auto application = submission;
    application.question_id = "integration-application-question";
    application.question_family_id = "integration.skill.application";
    application.evidence_type = QuestionEvidenceType::kApplication;
    application.transfer = true;
    const auto application_outcome = repository.RecordSubmission(application);
    Require(application_outcome.result == RecordSubmissionResult::kInserted && application_outcome.skill_mastery &&
                application_outcome.skill_mastery->previous == SkillMasteryStatus::kEncountered &&
                application_outcome.skill_mastery->current == SkillMasteryStatus::kDemonstrated &&
                application_outcome.skill_mastery->clean_family_count == 2 &&
                application_outcome.skill_mastery->transfer_family_count == 1 && application_outcome.xp_gained == 57,
            "an independent application family must confirm the skill");
    Require(repository.LoadCleanQuestionFamilies(user_id).contains("integration.skill.application"),
            "a clean application family must be available to the recommendation engine");

    auto same_family_variant = application;
    same_family_variant.question_id = "integration-application-variant";
    const auto variant_outcome = repository.RecordSubmission(same_family_variant);
    Require(variant_outcome.result == RecordSubmissionResult::kInserted && variant_outcome.skill_mastery &&
                variant_outcome.skill_mastery->current == SkillMasteryStatus::kDemonstrated &&
                variant_outcome.skill_mastery->clean_family_count == 2 && variant_outcome.xp_gained == 0,
            "a second question from the same family must not add independent evidence");
    Require(repository.LoadProgressSummary(user_id).xp_total == 69,
            "a repeated family must not change the accumulated XP");

    auto incomplete = submission;
    incomplete.question_id = "integration-incomplete-question";
    incomplete.skill_id = "integration.partial-skill";
    incomplete.question_family_id = "integration.partial-skill.first-contact";
    incomplete.correct = false;
    incomplete.completed = false;
    Require(repository.RecordSubmission(incomplete).result == RecordSubmissionResult::kInserted,
            "incomplete submission must be inserted");
    const auto after_incomplete = repository.LoadProgressSummary(user_id);
    const auto partial_skill =
        std::ranges::find(after_incomplete.skills, "integration.partial-skill", &SkillProgressSummary::skill_id);
    Require(partial_skill != after_incomplete.skills.end() && partial_skill->questions_seen == 1 &&
                partial_skill->completed_count == 0 && partial_skill->correct_count == 0 &&
                partial_skill->failed_count == 0,
            "skill coverage must include attempted but incomplete questions without counting a result");

    auto second_try = incomplete;
    second_try.correct = true;
    second_try.completed = true;
    second_try.attempt_number = 2;
    const auto second_try_outcome = repository.RecordSubmission(second_try);
    Require(second_try_outcome.result == RecordSubmissionResult::kInserted && second_try_outcome.xp_gained == 6,
            "a new family solved on the second attempt must award 6 XP");

    auto lower_level = submission;
    lower_level.question_id = "integration-lower-level-question";
    lower_level.question_family_id = "integration.skill.lower-level";
    lower_level.adaptive_level = 3;
    lower_level.adaptive_level_evidence = 0;
    Require(repository.RecordSubmission(lower_level).result == RecordSubmissionResult::kInserted,
            "lower-level practice submission must be inserted");
    Require(repository.GetAdaptiveLevel(user_id) == 3, "current adaptive level was not updated");
    Require(repository.GetUnlockedLevel(user_id) == 4, "a reached level must stay unlocked after demotion");

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

    const auto parallel_user_id = user_id - 1;
    const auto chat_session_id = repository.StartSession(parallel_user_id, "training:theory");
    const auto miniapp_session_id = repository.StartSession(parallel_user_id, "miniapp:training:theory");
    repository.FinishOpenSessionsWithModePrefix(parallel_user_id, "miniapp:", "integration-prefix-test");

    auto parallel_submission = submission;
    parallel_submission.user_id = parallel_user_id;
    parallel_submission.session_id = miniapp_session_id;
    parallel_submission.question_id = "miniapp-prefix-question";
    parallel_submission.question_family_id = "integration.skill.miniapp-prefix";
    rejected = false;
    try {
      static_cast<void>(repository.RecordSubmission(parallel_submission));
    } catch (const std::runtime_error&) {
      rejected = true;
    }
    Require(rejected, "mode-prefix cleanup left the Mini App session active");

    parallel_submission.session_id = chat_session_id;
    parallel_submission.question_id = "chat-prefix-question";
    parallel_submission.question_family_id = "integration.skill.chat-prefix";
    Require(repository.RecordSubmission(parallel_submission).result == RecordSubmissionResult::kInserted,
            "mode-prefix cleanup closed an unrelated chat session");
    repository.FinishSession(chat_session_id, "completed");

    const auto atomic_user_id = user_id - 2;
    const auto atomic_session_id = repository.StartSession(atomic_user_id, "integration-atomicity");
    auto atomic_submission = submission;
    atomic_submission.user_id = atomic_user_id;
    atomic_submission.session_id = atomic_session_id;
    atomic_submission.question_id = "integration-atomic-failure-question";
    atomic_submission.skill_id = "integration.atomic-skill";
    atomic_submission.question_family_id = "integration.atomic-skill.recognition";

    bool transaction_failed = false;
    {
      SubmissionFailureTrigger failure_trigger(database_url);
      try {
        static_cast<void>(repository.RecordSubmission(atomic_submission));
      } catch (const std::runtime_error&) {
        transaction_failed = true;
      }
    }
    Require(transaction_failed, "forced submission failure was not propagated");

    const auto after_rollback = repository.LoadProgressSummary(atomic_user_id);
    Require(after_rollback.questions_seen == 0 && after_rollback.completed_count == 0 && after_rollback.xp_total == 0 &&
                after_rollback.skills.empty(),
            "failed submission left partial attempt, progress, mastery or XP data");
    Require(!repository.LoadReviewStates(atomic_user_id).contains(std::string(atomic_submission.question_id)),
            "failed submission left partial review state");
    Require(!repository.LoadCleanQuestionFamilies(atomic_user_id)
                 .contains(std::string(atomic_submission.question_family_id)),
            "failed submission left partial skill evidence");

    const auto retry_outcome = repository.RecordSubmission(atomic_submission);
    Require(retry_outcome.result == RecordSubmissionResult::kInserted && retry_outcome.xp_gained == 12,
            "submission could not be retried cleanly after a transaction rollback");
    repository.FinishSession(atomic_session_id, "completed");
  } catch (const std::exception& error) {
    std::cerr << "storage_tests failed: " << error.what() << '\n';
    return 1;
  }

  std::cout << "storage_tests passed\n";
  return 0;
}
