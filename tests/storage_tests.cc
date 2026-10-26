#include "storage/attempt_repository.h"

#include <libpq-fe.h>

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <exception>
#include <future>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>

void CheckDatabaseTimeouts(const char* url);

namespace {

void Require(bool condition, std::string_view message) {
  if (!condition) {
    throw std::runtime_error(std::string(message));
  }
}

void ExecuteSql(const char* database_url, const std::string& sql) {
  PGconn* connection = PQconnectdb(database_url);
  if (connection == nullptr || PQstatus(connection) != CONNECTION_OK) {
    const std::string error = connection == nullptr ? "connection allocation failed" : PQerrorMessage(connection);
    if (connection != nullptr) {
      PQfinish(connection);
    }
    throw std::runtime_error("failed to prepare storage integration test: " + error);
  }

  PGresult* result = PQexec(connection, sql.c_str());
  const auto status = result == nullptr ? PGRES_FATAL_ERROR : PQresultStatus(result);
  const std::string error = status == PGRES_COMMAND_OK ? std::string{} : PQerrorMessage(connection);
  if (result != nullptr) {
    PQclear(result);
  }
  PQfinish(connection);
  if (status != PGRES_COMMAND_OK) {
    throw std::runtime_error("failed to prepare storage integration test: " + error);
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
    CheckDatabaseTimeouts(database_url);
    auto connection_pool = std::make_shared<DatabaseConnectionPool>(database_url, 2);
    Require(connection_pool->Size() == 2, "database connection pool has an unexpected size");
    AttemptRepository repository(connection_pool);
    AttemptRepository concurrent_repository(connection_pool);
    const auto timestamp = std::chrono::steady_clock::now().time_since_epoch().count();
    const auto user_id = -1 - static_cast<std::int64_t>(timestamp % 1'000'000'000);

    const auto concurrent_user_id = user_id - 20;
    const auto concurrent_session_id = repository.StartSession(concurrent_user_id, "integration-concurrent-pool");
    const SubmissionRecord concurrent_submission{
        .user_id = concurrent_user_id,
        .session_id = concurrent_session_id,
        .question_id = "integration-concurrent-question",
        .topic = "integration-concurrent-topic",
        .skill_id = "integration.concurrent-skill",
        .question_family_id = "integration.concurrent-skill.recognition",
        .evidence_type = QuestionEvidenceType::kRecognition,
        .transfer = false,
        .answer = "a",
        .correct = true,
        .completed = true,
        .attempt_number = 1,
        .topic_level = 3,
        .topic_level_evidence = 1,
        .adaptive_level = 3,
        .adaptive_level_evidence = 1,
    };
    std::promise<void> concurrent_start;
    const auto concurrent_start_signal = concurrent_start.get_future().share();
    auto first_concurrent = std::async(std::launch::async, [&] {
      concurrent_start_signal.wait();
      return repository.RecordSubmission(concurrent_submission);
    });
    auto second_concurrent = std::async(std::launch::async, [&] {
      concurrent_start_signal.wait();
      return concurrent_repository.RecordSubmission(concurrent_submission);
    });
    concurrent_start.set_value();
    const auto first_concurrent_outcome = first_concurrent.get();
    const auto second_concurrent_outcome = second_concurrent.get();
    Require((first_concurrent_outcome.result == RecordSubmissionResult::kInserted) !=
                (second_concurrent_outcome.result == RecordSubmissionResult::kInserted),
            "concurrent duplicate submissions were not reduced to one insert");
    const auto concurrent_summary = repository.LoadProgressSummary(concurrent_user_id);
    Require(concurrent_summary.completed_count == 1 && concurrent_summary.correct_count == 1 &&
                concurrent_summary.xp_total == 12,
            "concurrent duplicate submissions changed progress more than once");
    repository.FinishSession(concurrent_session_id, "completed");

    const auto active_user_id = user_id - 10;
    const auto active_session_id = repository.StartActiveTrainingSession(active_user_id, "miniapp:integration",
                                                                         "integration-content-v1", R"({"step":0})");
    const auto initial_active = repository.LoadActiveTrainingSession(active_user_id);
    Require(initial_active && initial_active->session_id == active_session_id && initial_active->state_version == 1 &&
                initial_active->content_version == "integration-content-v1",
            "active Mini App session was not stored");
    const auto updated_version = repository.UpdateActiveTrainingSession(active_user_id, active_session_id, 1,
                                                                        "integration-content-v1", R"({"step":1})");
    Require(updated_version == 2, "active Mini App session version was not advanced");

    bool stale_snapshot_rejected = false;
    try {
      static_cast<void>(repository.UpdateActiveTrainingSession(active_user_id, active_session_id, 1,
                                                               "integration-content-v1", R"({"step":"stale"})"));
    } catch (const std::logic_error&) {
      stale_snapshot_rejected = true;
    }
    Require(stale_snapshot_rejected, "a stale active Mini App snapshot overwrote newer state");

    const SubmissionRecord active_submission{
        .user_id = active_user_id,
        .session_id = active_session_id,
        .question_id = "active-session-question",
        .topic = "active-session-topic",
        .skill_id = "active-session.skill",
        .question_family_id = "active-session.skill.recognition",
        .evidence_type = QuestionEvidenceType::kRecognition,
        .transfer = false,
        .answer = "a",
        .correct = true,
        .completed = true,
        .attempt_number = 1,
        .topic_level = 3,
        .topic_level_evidence = 1,
        .adaptive_level = 3,
        .adaptive_level_evidence = 1,
    };
    const auto active_outcome = repository.RecordSubmission(
        active_submission, ActiveSessionUpdate{
                               .expected_version = 2,
                               .content_version = "integration-content-v1",
                               .build_state_json =
                                   [](const RecordSubmissionOutcome& outcome) {
                                     Require(
                                         outcome.result == RecordSubmissionResult::kInserted && outcome.xp_gained == 12,
                                         "submission outcome was unavailable while building the atomic snapshot");
                                     return std::string(R"({"step":2})");
                                   },
                           });
    Require(active_outcome.result == RecordSubmissionResult::kInserted, "active Mini App submission was not recorded");
    const auto answered_active = repository.LoadActiveTrainingSession(active_user_id);
    Require(answered_active && answered_active->state_version == 3 && answered_active->state_json.contains("2"),
            "answer progress and active session snapshot were not committed together");

    auto conflicting_submission = active_submission;
    conflicting_submission.question_id = "active-session-conflict-question";
    conflicting_submission.question_family_id = "active-session.skill.conflict";
    bool conflicting_submission_rejected = false;
    try {
      static_cast<void>(repository.RecordSubmission(
          conflicting_submission,
          ActiveSessionUpdate{
              .expected_version = 2,
              .content_version = "integration-content-v1",
              .build_state_json = [](const RecordSubmissionOutcome&) { return std::string(R"({"step":99})"); },
          }));
    } catch (const std::logic_error&) {
      conflicting_submission_rejected = true;
    }
    Require(conflicting_submission_rejected, "a submission with a stale session snapshot was accepted");
    Require(!repository.LoadReviewStates(active_user_id).contains("active-session-conflict-question"),
            "a rejected snapshot update left partially committed answer progress");

    repository.FinishSession(active_session_id, "completed");
    Require(!repository.LoadActiveTrainingSession(active_user_id), "finished Mini App session kept an active snapshot");

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

    const auto reward_user_id = user_id - 4;
    repository.EnsureUser(reward_user_id);
    bool reward_locked = false;
    try {
      repository.SetProfileFrame(reward_user_id, "cpp_blue");
    } catch (const std::invalid_argument&) {
      reward_locked = true;
    }
    Require(reward_locked, "a profile frame was selectable before its reward was unlocked");

    ExecuteSql(database_url,
               "INSERT INTO xp_ledger(user_id, event_type, source_id, cycle_id, delta, rules_version) VALUES (" +
                   std::to_string(reward_user_id) + ", 'integration_reward', 'profile', 'initial', 150, 1);");
    const auto first_reward_account = repository.LoadRewardAccount(reward_user_id);
    Require(first_reward_account.unlocked_reward_ids.contains("profile_frames") &&
                first_reward_account.newly_unlocked_reward_ids.size() == 1 &&
                first_reward_account.newly_unlocked_reward_ids.front() == "profile_frames" &&
                first_reward_account.preset_slot_count == 1,
            "past XP did not backfill the first functional reward");
    Require(repository.LoadRewardAccount(reward_user_id).newly_unlocked_reward_ids.empty(),
            "an already announced reward was announced twice");
    repository.SetProfileFrame(reward_user_id, "cpp_blue");
    repository.SaveTrainingPreset(reward_user_id, TrainingPreset{
                                                      .slot = 1,
                                                      .name = "Первый набор",
                                                      .modes = {"code"},
                                                      .topics = {"lifetime"},
                                                      .level = 3,
                                                  });
    reward_locked = false;
    try {
      repository.SaveTrainingPreset(reward_user_id, TrainingPreset{
                                                        .slot = 2,
                                                        .name = "Второй набор",
                                                        .modes = {"theory"},
                                                        .topics = {},
                                                        .level = 2,
                                                    });
    } catch (const std::invalid_argument&) {
      reward_locked = true;
    }
    Require(reward_locked, "the second preset slot was usable before its reward was unlocked");

    ExecuteSql(database_url,
               "INSERT INTO xp_ledger(user_id, event_type, source_id, cycle_id, delta, rules_version) VALUES (" +
                   std::to_string(reward_user_id) + ", 'integration_reward', 'second-preset', 'initial', 250, 1);");
    const auto second_reward_account = repository.LoadRewardAccount(reward_user_id);
    Require(second_reward_account.unlocked_reward_ids.contains("second_preset") &&
                second_reward_account.newly_unlocked_reward_ids.size() == 1 &&
                second_reward_account.newly_unlocked_reward_ids.front() == "second_preset" &&
                second_reward_account.profile_frame == "cpp_blue" && second_reward_account.preset_slot_count == 2,
            "past XP did not backfill the second preset reward");
    repository.SaveTrainingPreset(reward_user_id, TrainingPreset{
                                                      .slot = 2,
                                                      .name = "Второй набор",
                                                      .modes = {"theory", "api"},
                                                      .topics = {},
                                                      .level = 2,
                                                  });
    const auto saved_rewards = repository.LoadRewardAccount(reward_user_id);
    Require(saved_rewards.presets.size() == 2 && saved_rewards.presets[0].slot == 1 &&
                saved_rewards.presets[0].modes == std::vector<std::string>{"code"} &&
                saved_rewards.presets[1].slot == 2 && saved_rewards.presets[1].modes.size() == 2,
            "unlocked training presets were not persisted");

    const auto crossing_user_id = user_id - 5;
    repository.EnsureUser(crossing_user_id);
    ExecuteSql(database_url,
               "INSERT INTO xp_ledger(user_id, event_type, source_id, cycle_id, delta, rules_version) VALUES (" +
                   std::to_string(crossing_user_id) + ", 'integration_reward', 'seed', 'initial', 145, 1);");
    const auto crossing_session_id = repository.StartSession(crossing_user_id, "integration-reward-crossing");
    auto crossing_submission = submission;
    crossing_submission.user_id = crossing_user_id;
    crossing_submission.session_id = crossing_session_id;
    crossing_submission.question_id = "integration-reward-crossing-question";
    crossing_submission.skill_id = "integration.reward-crossing";
    crossing_submission.question_family_id = "integration.reward-crossing.family";
    const auto crossing_outcome = repository.RecordSubmission(crossing_submission);
    Require(crossing_outcome.xp_gained == 12 && crossing_outcome.rewards_unlocked.size() == 1 &&
                crossing_outcome.rewards_unlocked.front() == "profile_frames",
            "crossing an XP threshold did not unlock the reward in the submission transaction");
    repository.FinishSession(crossing_session_id, "completed");

    const auto mastery_user_id = user_id - 3;
    const auto mastery_session_id = repository.StartSession(mastery_user_id, "integration-mastery");
    auto mastery_recognition = submission;
    mastery_recognition.user_id = mastery_user_id;
    mastery_recognition.session_id = mastery_session_id;
    mastery_recognition.question_id = "integration-mastery-recognition";
    mastery_recognition.skill_id = "integration.mastery-skill";
    mastery_recognition.question_family_id = "integration.mastery-skill.recognition";
    Require(repository.RecordSubmission(mastery_recognition).result == RecordSubmissionResult::kInserted,
            "mastery recognition evidence was not inserted");

    auto mastery_application = mastery_recognition;
    mastery_application.question_id = "integration-mastery-application";
    mastery_application.question_family_id = "integration.mastery-skill.application";
    mastery_application.evidence_type = QuestionEvidenceType::kApplication;
    mastery_application.transfer = true;
    const auto mastery_demonstrated = repository.RecordSubmission(mastery_application);
    Require(mastery_demonstrated.skill_mastery &&
                mastery_demonstrated.skill_mastery->current == SkillMasteryStatus::kDemonstrated,
            "independent application evidence did not demonstrate the integration skill");

    repository.FinishSession(mastery_session_id, "completed");

    ExecuteSql(database_url,
               "UPDATE user_skill_state SET demonstrated_at = now() - INTERVAL '2 days' WHERE user_id = " +
                   std::to_string(mastery_user_id) + " AND skill_id = 'integration.mastery-skill';");
    const auto retention_session_id = repository.StartSession(mastery_user_id, "integration-mastery-retention");
    auto same_family_delayed = mastery_application;
    same_family_delayed.session_id = retention_session_id;
    same_family_delayed.question_id = "integration-mastery-application-variant";
    const auto same_family_outcome = repository.RecordSubmission(same_family_delayed);
    Require(same_family_outcome.skill_mastery &&
                same_family_outcome.skill_mastery->current == SkillMasteryStatus::kRetained,
            "a delayed application from an existing family did not retain the skill");
    repository.FinishSession(retention_session_id, "completed");

    const auto review_session_id = repository.StartSession(mastery_user_id, "integration-mastery-review");
    ExecuteSql(database_url,
               "UPDATE question_progress SET review_stage = 5, consecutive_correct = 5, "
               "next_review_at = now() - INTERVAL '1 minute' WHERE user_id = " +
                   std::to_string(mastery_user_id) + " AND question_id = 'integration-mastery-application-variant';");

    auto failed_first_attempt = same_family_delayed;
    failed_first_attempt.session_id = review_session_id;
    failed_first_attempt.correct = false;
    failed_first_attempt.completed = false;
    const auto before_review_failure = repository.LoadProgressSummary(mastery_user_id);
    bool failed_snapshot_rejected = false;
    try {
      static_cast<void>(repository.RecordSubmission(
          failed_first_attempt,
          {.expected_version = 1, .content_version = "test", .build_state_json = [](const RecordSubmissionOutcome&) {
             return std::string(R"({"step":1})");
           }}));
    } catch (const std::logic_error&) {
      failed_snapshot_rejected = true;
    }
    const auto rolled_back_failure = repository.LoadProgressSummary(mastery_user_id);
    Require(failed_snapshot_rejected &&
                rolled_back_failure.answers.attempts == before_review_failure.answers.attempts &&
                rolled_back_failure.skills.front().mastery_status == SkillMasteryStatus::kRetained &&
                repository.LoadReviewStates(mastery_user_id).at("integration-mastery-application-variant").stage == 5,
            "a failed snapshot write partially committed review failure");
    const auto failed_outcome = repository.RecordSubmission(failed_first_attempt);
    Require(failed_outcome.result == RecordSubmissionResult::kInserted && failed_outcome.xp_gained == 0 &&
                failed_outcome.skill_mastery &&
                failed_outcome.skill_mastery->previous == SkillMasteryStatus::kRetained &&
                failed_outcome.skill_mastery->current == SkillMasteryStatus::kNeedsRecovery,
            "an unfinished failed review did not immediately request recovery");
    Require(repository.RecordSubmission(failed_first_attempt).result == RecordSubmissionResult::kDuplicate,
            "a duplicate failure was recorded twice");
    AttemptRepository restarted_repository(connection_pool);
    const auto after_review_failure = restarted_repository.LoadProgressSummary(mastery_user_id);
    const auto failed_review_state =
        restarted_repository.LoadReviewStates(mastery_user_id).at("integration-mastery-application-variant");
    Require(after_review_failure.skills.front().mastery_status == SkillMasteryStatus::kNeedsRecovery &&
                after_review_failure.xp_total == before_review_failure.xp_total &&
                after_review_failure.completed_count == before_review_failure.completed_count &&
                after_review_failure.skills.front().clean_family_count ==
                    before_review_failure.skills.front().clean_family_count &&
                failed_review_state.stage == 1 && failed_review_state.due,
            "abandoning an incorrect review lost recovery, evidence, XP or its pending due cycle");

    auto corrected_second_attempt = failed_first_attempt;
    corrected_second_attempt.correct = true;
    corrected_second_attempt.completed = true;
    corrected_second_attempt.attempt_number = 2;
    const auto recovery_required = repository.RecordSubmission(corrected_second_attempt);
    Require(recovery_required.skill_mastery &&
                recovery_required.skill_mastery->current == SkillMasteryStatus::kNeedsRecovery &&
                recovery_required.xp_gained == 8,
            "a due review solved only on the second attempt did not request recovery");
    const auto repaired_state =
        repository.LoadReviewStates(mastery_user_id).at("integration-mastery-application-variant");
    Require(repaired_state.stage == 1 && repaired_state.seconds_until_due >= 86'390 &&
                repaired_state.seconds_until_due <= 86'400,
            "a corrected review retained its old long stage or missed its one-day interval");

    auto repeated_recovery = mastery_application;
    repeated_recovery.session_id = review_session_id;
    repeated_recovery.question_id = "integration-mastery-repeated-recovery";
    const auto repeated_recovery_outcome = repository.RecordSubmission(repeated_recovery);
    Require(repeated_recovery_outcome.skill_mastery &&
                repeated_recovery_outcome.skill_mastery->current == SkillMasteryStatus::kDemonstrated,
            "a clean application after recovery started did not restore demonstrated status");

    auto immediate_retention = mastery_application;
    immediate_retention.session_id = review_session_id;
    immediate_retention.question_id = "integration-mastery-immediate-retention";
    const auto immediate_retention_outcome = repository.RecordSubmission(immediate_retention);
    Require(immediate_retention_outcome.skill_mastery &&
                immediate_retention_outcome.skill_mastery->current == SkillMasteryStatus::kDemonstrated,
            "historical retained evidence was reused immediately after recovery");
    repository.FinishSession(review_session_id, "completed");

    ExecuteSql(database_url,
               "UPDATE user_skill_state SET demonstrated_at = now() - INTERVAL '2 days' WHERE user_id = " +
                   std::to_string(mastery_user_id) + " AND skill_id = 'integration.mastery-skill';");
    ExecuteSql(database_url,
               "UPDATE question_progress SET next_review_at = now() - INTERVAL '1 minute' WHERE user_id = " +
                   std::to_string(mastery_user_id) + " AND question_id = 'integration-mastery-immediate-retention';");
    const auto second_retention_session_id =
        repository.StartSession(mastery_user_id, "integration-mastery-second-retention");
    auto second_retention = mastery_application;
    second_retention.session_id = second_retention_session_id;
    second_retention.question_id = "integration-mastery-immediate-retention";
    const auto second_retention_outcome = repository.RecordSubmission(second_retention);
    Require(second_retention_outcome.skill_mastery &&
                second_retention_outcome.skill_mastery->current == SkillMasteryStatus::kRetained,
            "a recovered skill could not be retained by a due application in a later session");
    repository.FinishSession(second_retention_session_id, "completed");

    ExecuteSql(database_url,
               "UPDATE question_progress SET next_review_at = now() - INTERVAL '1 minute' WHERE user_id = " +
                   std::to_string(mastery_user_id) + " AND question_id = 'integration-mastery-application-variant';");
    auto next_clean_review = same_family_delayed;
    next_clean_review.session_id = repository.StartSession(mastery_user_id, "integration-repaired-review");
    static_cast<void>(repository.RecordSubmission(next_clean_review));
    const auto next_review_state =
        repository.LoadReviewStates(mastery_user_id).at("integration-mastery-application-variant");
    Require(next_review_state.stage == 2 && next_review_state.seconds_until_due >= 259'190 &&
                next_review_state.seconds_until_due <= 259'200,
            "a clean answer following repair immediately returned to the old 30-day interval");
    repository.FinishSession(next_clean_review.session_id, "completed");
  } catch (const std::exception& error) {
    std::cerr << "storage_tests failed: " << error.what() << '\n';
    return 1;
  }

  std::cout << "storage_tests passed\n";
  return 0;
}
