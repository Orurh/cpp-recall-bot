#include "attempt_repository.h"

#include <libpq-fe.h>

#include <algorithm>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <unordered_map>
#include <utility>
#include <vector>

namespace {

class Result {
 public:
  explicit Result(PGresult* result) : result_(result) {
    if (result_ == nullptr) {
      throw std::runtime_error("PostgreSQL returned null result");
    }
  }

  ~Result() { PQclear(result_); }

  Result(const Result&) = delete;
  Result& operator=(const Result&) = delete;

  Result(Result&& other) noexcept : result_(std::exchange(other.result_, nullptr)) {}

  Result& operator=(Result&& other) noexcept {
    if (this == &other) {
      return *this;
    }

    PQclear(result_);

    result_ = std::exchange(other.result_, nullptr);

    return *this;
  }

  [[nodiscard]]
  PGresult* Get() const noexcept {
    return result_;
  }

 private:
  PGresult* result_ = nullptr;
};

[[noreturn]]
void ThrowDatabaseError(PGconn* connection, std::string_view operation) {
  std::string message(operation);
  message += ": ";

  if (connection != nullptr) {
    message += PQerrorMessage(connection);
  } else {
    message += "unknown PostgreSQL error";
  }

  throw std::runtime_error(message);
}

void CheckCommand(PGconn* connection, const Result& result, std::string_view operation) {
  if (PQresultStatus(result.Get()) != PGRES_COMMAND_OK) {
    ThrowDatabaseError(connection, operation);
  }
}

void CheckQuery(PGconn* connection, const Result& result, std::string_view operation) {
  if (PQresultStatus(result.Get()) != PGRES_TUPLES_OK) {
    ThrowDatabaseError(connection, operation);
  }
}

void EnsureConnection(PGconn* connection) {
  if (connection == nullptr) {
    ThrowDatabaseError(connection, "PostgreSQL connection is not initialized");
  }

  if (PQstatus(connection) == CONNECTION_OK) {
    return;
  }

  PQreset(connection);
  if (PQstatus(connection) != CONNECTION_OK) {
    ThrowDatabaseError(connection, "failed to reconnect to PostgreSQL");
  }
}

void Execute(PGconn* connection, std::string_view sql) {
  EnsureConnection(connection);
  const std::string sql_text(sql);
  Result result(PQexec(connection, sql_text.c_str()));

  CheckCommand(connection, result, "PostgreSQL command failed");
}

Result ExecuteParams(PGconn* connection, const char* sql, const std::vector<std::string>& params, bool expect_rows) {
  EnsureConnection(connection);
  std::vector<const char*> values;
  values.reserve(params.size());

  for (const auto& param : params) {
    values.push_back(param.c_str());
  }

  Result result(
      PQexecParams(connection, sql, static_cast<int>(values.size()), nullptr, values.data(), nullptr, nullptr, 0));

  if (expect_rows) {
    CheckQuery(connection, result, "PostgreSQL query failed");
  } else {
    CheckCommand(connection, result, "PostgreSQL command failed");
  }

  return result;
}

std::int64_t ParseInt64(const char* value) {
  if (value == nullptr) {
    throw std::runtime_error("PostgreSQL returned null integer");
  }

  const std::string_view text(value);

  std::int64_t result = 0;

  const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), result);

  if (error != std::errc{} || end != text.data() + text.size()) {
    throw std::runtime_error("PostgreSQL returned invalid integer");
  }

  return result;
}

bool ParseBool(const char* value) {
  if (value == nullptr) {
    throw std::runtime_error("PostgreSQL returned null boolean");
  }

  const std::string_view text(value);

  if (text == "t" || text == "true") {
    return true;
  }

  if (text == "f" || text == "false") {
    return false;
  }

  throw std::runtime_error("PostgreSQL returned invalid boolean");
}

void ValidateProgressCount(std::int64_t value, std::string_view name) {
  if (value < 0) {
    throw std::runtime_error("stored " + std::string(name) + " must not be negative");
  }
}

std::string ReadFile(const std::filesystem::path& path) {
  std::ifstream input(path, std::ios::binary);
  if (!input) {
    throw std::runtime_error("failed to open migration: " + path.string());
  }

  return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

void ApplyMigrations(PGconn* connection, const std::filesystem::path& directory) {
  Execute(connection,
          R"sql(
CREATE TABLE IF NOT EXISTS schema_migrations (
    version TEXT PRIMARY KEY,
    applied_at TIMESTAMPTZ NOT NULL DEFAULT now()
);
)sql");

  if (!std::filesystem::is_directory(directory)) {
    throw std::runtime_error("migration directory not found: " + directory.string());
  }

  std::vector<std::filesystem::path> migrations;
  for (const auto& entry : std::filesystem::directory_iterator(directory)) {
    if (entry.is_regular_file() && entry.path().extension() == ".sql") {
      migrations.push_back(entry.path());
    }
  }

  std::ranges::sort(migrations);
  if (migrations.empty()) {
    throw std::runtime_error("no SQL migrations found in: " + directory.string());
  }

  for (const auto& migration : migrations) {
    const auto version = migration.filename().string();
    Execute(connection, "BEGIN;");
    try {
      static_cast<void>(ExecuteParams(connection, "SELECT pg_advisory_xact_lock(1936028748);", {}, true));

      const auto applied = ExecuteParams(
          connection, "SELECT EXISTS(SELECT 1 FROM schema_migrations WHERE version = $1);", {version}, true);

      if (PQntuples(applied.Get()) != 1 || PQnfields(applied.Get()) != 1) {
        throw std::runtime_error("unexpected migration lookup result");
      }
      if (ParseBool(PQgetvalue(applied.Get(), 0, 0))) {
        Execute(connection, "COMMIT;");
        continue;
      }

      Execute(connection, ReadFile(migration));
      ExecuteParams(connection, "INSERT INTO schema_migrations(version) VALUES ($1);", {version}, false);
      Execute(connection, "COMMIT;");
    } catch (...) {
      try {
        Execute(connection, "ROLLBACK;");
      } catch (...) {}
      throw;
    }
  }
}

}  // namespace

struct AttemptRepository::Impl {
  explicit Impl(const std::string& database_url) : connection(PQconnectdb(database_url.c_str())) {

    if (connection == nullptr) {
      throw std::runtime_error("failed to allocate PostgreSQL connection");
    }

    if (PQstatus(connection) != CONNECTION_OK) {
      const std::string error = PQerrorMessage(connection);

      PQfinish(connection);
      connection = nullptr;

      throw std::runtime_error("failed to connect to PostgreSQL: " + error);
    }

    ApplyMigrations(connection, "migrations");
  }

  ~Impl() {
    if (connection != nullptr) {
      PQfinish(connection);
    }
  }

  PGconn* connection = nullptr;
};

AttemptRepository::AttemptRepository(std::string database_url)
    : impl_(std::make_unique<Impl>(std::move(database_url))) {}

AttemptRepository::~AttemptRepository() = default;

AttemptRepository::AttemptRepository(AttemptRepository&&) noexcept = default;

AttemptRepository& AttemptRepository::operator=(AttemptRepository&&) noexcept = default;

void AttemptRepository::EnsureUser(std::int64_t user_id) {
  ExecuteParams(impl_->connection,
                R"sql(
INSERT INTO users(user_id)
VALUES ($1)
ON CONFLICT (user_id) DO NOTHING;
)sql",
                {
                    std::to_string(user_id),
                },
                false);
}

AdaptiveLevelState AttemptRepository::GetAdaptiveState(std::int64_t user_id) {
  EnsureUser(user_id);

  const auto result = ExecuteParams(impl_->connection,
                                    R"sql(
SELECT adaptive_level, adaptive_evidence
FROM users
WHERE user_id = $1;
)sql",
                                    {
                                        std::to_string(user_id),
                                    },
                                    true);

  if (PQntuples(result.Get()) != 1 || PQnfields(result.Get()) != 2) {
    throw std::runtime_error("unexpected PostgreSQL result when reading adaptive state");
  }

  const AdaptiveLevelState state{
      .level = static_cast<int>(ParseInt64(PQgetvalue(result.Get(), 0, 0))),
      .evidence = static_cast<int>(ParseInt64(PQgetvalue(result.Get(), 0, 1))),
  };

  try {
    ValidateAdaptiveLevelState(state);
  } catch (const std::invalid_argument& error) {
    throw std::runtime_error(std::string("invalid stored adaptive state: ") + error.what());
  }

  return state;
}

int AttemptRepository::GetAdaptiveLevel(std::int64_t user_id) {
  return GetAdaptiveState(user_id).level;
}

int AttemptRepository::GetUnlockedLevel(std::int64_t user_id) {
  EnsureUser(user_id);

  const auto result = ExecuteParams(impl_->connection,
                                    R"sql(
SELECT unlocked_level
FROM users
WHERE user_id = $1;
)sql",
                                    {
                                        std::to_string(user_id),
                                    },
                                    true);

  if (PQntuples(result.Get()) != 1 || PQnfields(result.Get()) != 1) {
    throw std::runtime_error("unexpected PostgreSQL result when reading unlocked level");
  }

  const auto level = static_cast<int>(ParseInt64(PQgetvalue(result.Get(), 0, 0)));
  if (level < 1 || level > 10) {
    throw std::runtime_error("stored unlocked level must be between 1 and 10");
  }
  return level;
}

std::unordered_map<std::string, AdaptiveLevelState> AttemptRepository::LoadTopicStates(std::int64_t user_id) {
  EnsureUser(user_id);

  const auto result = ExecuteParams(impl_->connection,
                                    R"sql(
SELECT topic, adaptive_level, adaptive_evidence
FROM topic_progress
WHERE user_id = $1;
)sql",
                                    {
                                        std::to_string(user_id),
                                    },
                                    true);

  std::unordered_map<std::string, AdaptiveLevelState> states;

  const auto row_count = PQntuples(result.Get());

  states.reserve(static_cast<std::size_t>(row_count));

  for (int row = 0; row < row_count; ++row) {
    const auto* topic_value = PQgetvalue(result.Get(), row, 0);

    if (topic_value == nullptr || *topic_value == '\0') {
      throw std::runtime_error("stored topic must not be empty");
    }

    const AdaptiveLevelState state{
        .level = static_cast<int>(ParseInt64(PQgetvalue(result.Get(), row, 1))),
        .evidence = static_cast<int>(ParseInt64(PQgetvalue(result.Get(), row, 2))),
    };

    try {
      ValidateAdaptiveLevelState(state);
    } catch (const std::invalid_argument& error) {
      throw std::runtime_error(std::string("invalid stored topic state: ") + error.what());
    }

    states.emplace(topic_value, state);
  }

  return states;
}

std::unordered_map<std::string, int> AttemptRepository::LoadTopicLevels(std::int64_t user_id) {
  const auto states = LoadTopicStates(user_id);
  std::unordered_map<std::string, int> levels;
  levels.reserve(states.size());

  for (const auto& [topic, state] : states) {
    levels.emplace(topic, state.level);
  }

  return levels;
}

std::unordered_map<std::string, QuestionReviewState> AttemptRepository::LoadReviewStates(std::int64_t user_id) {
  EnsureUser(user_id);

  const auto result = ExecuteParams(impl_->connection,
                                    R"sql(
SELECT
    question_id,
    next_review_at <= now(),
    review_stage,
    GREATEST(
        0,
        EXTRACT(EPOCH FROM (next_review_at - now()))::BIGINT
    ),
    correct_count,
    failed_count
FROM question_progress
WHERE user_id = $1;
)sql",
                                    {
                                        std::to_string(user_id),
                                    },
                                    true);

  std::unordered_map<std::string, QuestionReviewState> states;
  const auto row_count = PQntuples(result.Get());
  states.reserve(static_cast<std::size_t>(row_count));

  for (int row = 0; row < row_count; ++row) {
    const auto* question_id = PQgetvalue(result.Get(), row, 0);

    if (question_id == nullptr || *question_id == '\0') {
      throw std::runtime_error("stored question id must not be empty");
    }

    const auto stage = ParseInt64(PQgetvalue(result.Get(), row, 2));

    if (stage < 0 || stage > 5) {
      throw std::runtime_error("stored review stage must be between 0 and 5");
    }

    states.emplace(question_id, QuestionReviewState{
                                    .due = ParseBool(PQgetvalue(result.Get(), row, 1)),
                                    .stage = static_cast<int>(stage),
                                    .seconds_until_due = ParseInt64(PQgetvalue(result.Get(), row, 3)),
                                    .correct_count = ParseInt64(PQgetvalue(result.Get(), row, 4)),
                                    .failed_count = ParseInt64(PQgetvalue(result.Get(), row, 5)),
                                });
  }

  return states;
}

UserProgressSummary AttemptRepository::LoadProgressSummary(std::int64_t user_id) {
  EnsureUser(user_id);

  const auto user_id_text = std::to_string(user_id);
  const auto totals = ExecuteParams(impl_->connection,
                                    R"sql(
SELECT
    users.adaptive_level,
    users.adaptive_evidence,
    users.unlocked_level,
    (SELECT COUNT(*) FROM sessions WHERE sessions.user_id = users.user_id),
    (
        SELECT COUNT(*)
        FROM sessions
        WHERE sessions.user_id = users.user_id
          AND sessions.finish_reason = 'completed'
    ),
    (SELECT COUNT(*) FROM question_progress WHERE question_progress.user_id = users.user_id),
    (
        SELECT COALESCE(SUM(completed_count), 0)
        FROM question_progress
        WHERE question_progress.user_id = users.user_id
    ),
    (
        SELECT COALESCE(SUM(correct_count), 0)
        FROM question_progress
        WHERE question_progress.user_id = users.user_id
    ),
    (
        SELECT COALESCE(SUM(failed_count), 0)
        FROM question_progress
        WHERE question_progress.user_id = users.user_id
    ),
    (
        SELECT COALESCE(SUM(first_try_correct_count), 0)
        FROM question_progress
        WHERE question_progress.user_id = users.user_id
    ),
    (
        SELECT COUNT(*)
        FROM question_progress
        WHERE question_progress.user_id = users.user_id
          AND question_progress.next_review_at <= now()
    )
FROM users
WHERE users.user_id = $1;
)sql",
                                    {user_id_text}, true);

  if (PQntuples(totals.Get()) != 1 || PQnfields(totals.Get()) != 11) {
    throw std::runtime_error("unexpected PostgreSQL result when reading progress summary");
  }

  UserProgressSummary summary{
      .adaptive_level = static_cast<int>(ParseInt64(PQgetvalue(totals.Get(), 0, 0))),
      .adaptive_evidence = static_cast<int>(ParseInt64(PQgetvalue(totals.Get(), 0, 1))),
      .unlocked_level = static_cast<int>(ParseInt64(PQgetvalue(totals.Get(), 0, 2))),
      .sessions_started = ParseInt64(PQgetvalue(totals.Get(), 0, 3)),
      .sessions_completed = ParseInt64(PQgetvalue(totals.Get(), 0, 4)),
      .questions_seen = ParseInt64(PQgetvalue(totals.Get(), 0, 5)),
      .completed_count = ParseInt64(PQgetvalue(totals.Get(), 0, 6)),
      .correct_count = ParseInt64(PQgetvalue(totals.Get(), 0, 7)),
      .failed_count = ParseInt64(PQgetvalue(totals.Get(), 0, 8)),
      .first_try_correct_count = ParseInt64(PQgetvalue(totals.Get(), 0, 9)),
      .due_review_count = ParseInt64(PQgetvalue(totals.Get(), 0, 10)),
      .topics = {},
      .skills = {},
  };

  if (summary.adaptive_level < 1 || summary.adaptive_level > 10) {
    throw std::runtime_error("stored adaptive level must be between 1 and 10");
  }

  if (summary.unlocked_level < summary.adaptive_level || summary.unlocked_level > 10) {
    throw std::runtime_error("stored unlocked level must contain the adaptive level and be at most 10");
  }

  ValidateAdaptiveLevelState({.level = summary.adaptive_level, .evidence = summary.adaptive_evidence});

  ValidateProgressCount(summary.sessions_started, "sessions started");
  ValidateProgressCount(summary.sessions_completed, "sessions completed");
  ValidateProgressCount(summary.questions_seen, "questions seen");
  ValidateProgressCount(summary.completed_count, "completed count");
  ValidateProgressCount(summary.correct_count, "correct count");
  ValidateProgressCount(summary.failed_count, "failed count");
  ValidateProgressCount(summary.first_try_correct_count, "first-try correct count");
  ValidateProgressCount(summary.due_review_count, "due review count");

  const auto topics = ExecuteParams(impl_->connection,
                                    R"sql(
SELECT
    topic,
    adaptive_level,
    adaptive_evidence,
    completed_count,
    correct_count,
    failed_count,
    first_try_correct_count
FROM topic_progress
WHERE user_id = $1
ORDER BY completed_count DESC, topic;
)sql",
                                    {user_id_text}, true);

  const auto topic_count = PQntuples(topics.Get());
  summary.topics.reserve(static_cast<std::size_t>(topic_count));

  for (int row = 0; row < topic_count; ++row) {
    const auto* topic = PQgetvalue(topics.Get(), row, 0);

    if (topic == nullptr || *topic == '\0') {
      throw std::runtime_error("stored topic must not be empty");
    }

    TopicProgressSummary topic_summary{
        .topic = topic,
        .adaptive_level = static_cast<int>(ParseInt64(PQgetvalue(topics.Get(), row, 1))),
        .adaptive_evidence = static_cast<int>(ParseInt64(PQgetvalue(topics.Get(), row, 2))),
        .completed_count = ParseInt64(PQgetvalue(topics.Get(), row, 3)),
        .correct_count = ParseInt64(PQgetvalue(topics.Get(), row, 4)),
        .failed_count = ParseInt64(PQgetvalue(topics.Get(), row, 5)),
        .first_try_correct_count = ParseInt64(PQgetvalue(topics.Get(), row, 6)),
    };

    if (topic_summary.adaptive_level < 1 || topic_summary.adaptive_level > 10) {
      throw std::runtime_error("stored topic level must be between 1 and 10");
    }

    ValidateAdaptiveLevelState({.level = topic_summary.adaptive_level, .evidence = topic_summary.adaptive_evidence});

    ValidateProgressCount(topic_summary.completed_count, "topic completed count");
    ValidateProgressCount(topic_summary.correct_count, "topic correct count");
    ValidateProgressCount(topic_summary.failed_count, "topic failed count");
    ValidateProgressCount(topic_summary.first_try_correct_count, "topic first-try correct count");

    summary.topics.push_back(std::move(topic_summary));
  }

  const auto skills = ExecuteParams(impl_->connection,
                                    R"sql(
SELECT
    skill_id,
    COUNT(*),
    COALESCE(SUM(completed_count), 0),
    COALESCE(SUM(correct_count), 0),
    COALESCE(SUM(failed_count), 0),
    COALESCE(SUM(first_try_correct_count), 0)
FROM skill_progress
WHERE user_id = $1
GROUP BY skill_id
ORDER BY COUNT(*) DESC, skill_id;
)sql",
                                    {user_id_text}, true);

  const auto skill_count = PQntuples(skills.Get());
  summary.skills.reserve(static_cast<std::size_t>(skill_count));

  for (int row = 0; row < skill_count; ++row) {
    const auto* skill_id = PQgetvalue(skills.Get(), row, 0);
    if (skill_id == nullptr || *skill_id == '\0') {
      throw std::runtime_error("stored skill id must not be empty");
    }

    SkillProgressSummary skill_summary{
        .skill_id = skill_id,
        .questions_seen = ParseInt64(PQgetvalue(skills.Get(), row, 1)),
        .completed_count = ParseInt64(PQgetvalue(skills.Get(), row, 2)),
        .correct_count = ParseInt64(PQgetvalue(skills.Get(), row, 3)),
        .failed_count = ParseInt64(PQgetvalue(skills.Get(), row, 4)),
        .first_try_correct_count = ParseInt64(PQgetvalue(skills.Get(), row, 5)),
    };
    ValidateProgressCount(skill_summary.questions_seen, "skill questions seen");
    ValidateProgressCount(skill_summary.completed_count, "skill completed count");
    ValidateProgressCount(skill_summary.correct_count, "skill correct count");
    ValidateProgressCount(skill_summary.failed_count, "skill failed count");
    ValidateProgressCount(skill_summary.first_try_correct_count, "skill first-try correct count");
    summary.skills.push_back(std::move(skill_summary));
  }

  return summary;
}

std::int64_t AttemptRepository::StartSession(std::int64_t user_id, std::string_view mode) {
  EnsureUser(user_id);

  const auto result = ExecuteParams(impl_->connection,
                                    R"sql(
INSERT INTO sessions(user_id, mode)
VALUES ($1, $2)
RETURNING id;
)sql",
                                    {
                                        std::to_string(user_id),
                                        std::string(mode),
                                    },
                                    true);

  if (PQntuples(result.Get()) != 1 || PQnfields(result.Get()) != 1) {
    throw std::runtime_error(
        "unexpected PostgreSQL result "
        "when starting session");
  }

  return ParseInt64(PQgetvalue(result.Get(), 0, 0));
}

RecordSubmissionResult AttemptRepository::RecordSubmission(const SubmissionRecord& submission) {
  if (submission.user_id == 0 || submission.session_id == 0) {
    throw std::invalid_argument("user and session ids must not be zero");
  }

  if (submission.question_id.empty() || submission.skill_id.empty() || submission.answer.empty()) {
    throw std::invalid_argument("question id, skill id and answer must not be empty");
  }

  if (submission.attempt_number < 1 || submission.attempt_number > 3) {
    throw std::invalid_argument("attempt number must be between 1 and 3");
  }

  if (submission.completed && submission.topic.empty()) {
    throw std::invalid_argument("completed submission topic must not be empty");
  }

  if (submission.topic_level < 1 || submission.topic_level > 10 || submission.adaptive_level < 1 ||
      submission.adaptive_level > 10) {
    throw std::invalid_argument("adaptive levels must be between 1 and 10");
  }

  ValidateAdaptiveLevelState({.level = submission.topic_level, .evidence = submission.topic_level_evidence});
  ValidateAdaptiveLevelState({.level = submission.adaptive_level, .evidence = submission.adaptive_level_evidence});

  const auto user_id = std::to_string(submission.user_id);
  const auto session_id = std::to_string(submission.session_id);
  const auto attempt_number = std::to_string(submission.attempt_number);
  const auto correct = submission.correct ? "true" : "false";
  const auto completed_increment = submission.completed ? "1" : "0";
  const auto correct_increment = submission.completed && submission.correct ? "1" : "0";
  const auto failed_increment = submission.completed && !submission.correct ? "1" : "0";
  const auto first_try_increment =
      submission.completed && submission.correct && submission.attempt_number == 1 ? "1" : "0";

  Execute(impl_->connection, "BEGIN");

  try {
    ExecuteParams(impl_->connection,
                  R"sql(
INSERT INTO users(user_id)
VALUES ($1)
ON CONFLICT (user_id) DO NOTHING;
)sql",
                  {user_id}, false);

    const auto attempt = ExecuteParams(impl_->connection,
                                       R"sql(
INSERT INTO attempts(
    user_id,
    session_id,
    question_id,
    answer,
    correct,
    attempt_number
)
SELECT $1, $2, $3, $4, $5, $6
FROM sessions
WHERE id = $2
  AND user_id = $1
  AND finished_at IS NULL
ON CONFLICT (session_id, question_id, attempt_number)
DO NOTHING
RETURNING id;
)sql",
                                       {
                                           user_id,
                                           session_id,
                                           std::string(submission.question_id),
                                           std::string(submission.answer),
                                           correct,
                                           attempt_number,
                                       },
                                       true);

    if (PQntuples(attempt.Get()) == 0) {
      const auto existing = ExecuteParams(impl_->connection,
                                          R"sql(
SELECT 1
FROM attempts
WHERE session_id = $1
  AND question_id = $2
  AND attempt_number = $3;
)sql",
                                          {
                                              session_id,
                                              std::string(submission.question_id),
                                              attempt_number,
                                          },
                                          true);

      if (PQntuples(existing.Get()) == 0) {
        throw std::runtime_error("submission references an inactive or foreign session");
      }

      Execute(impl_->connection, "COMMIT");
      return RecordSubmissionResult::kDuplicate;
    }

    ExecuteParams(impl_->connection,
                  R"sql(
INSERT INTO skill_progress AS progress(
    user_id,
    skill_id,
    question_id,
    completed_count,
    correct_count,
    failed_count,
    first_try_correct_count,
    last_answered_at
)
VALUES ($1, $2, $3, $4, $5, $6, $7, now())
ON CONFLICT (user_id, skill_id, question_id)
DO UPDATE SET
    completed_count = progress.completed_count + EXCLUDED.completed_count,
    correct_count = progress.correct_count + EXCLUDED.correct_count,
    failed_count = progress.failed_count + EXCLUDED.failed_count,
    first_try_correct_count =
        progress.first_try_correct_count + EXCLUDED.first_try_correct_count,
    last_answered_at = now();
)sql",
                  {
                      user_id,
                      std::string(submission.skill_id),
                      std::string(submission.question_id),
                      completed_increment,
                      correct_increment,
                      failed_increment,
                      first_try_increment,
                  },
                  false);

    if (submission.completed) {
      ExecuteParams(impl_->connection,
                    R"sql(
INSERT INTO question_progress AS progress(
    user_id,
    question_id,
    completed_count,
    correct_count,
    failed_count,
    first_try_correct_count,
    review_stage,
    consecutive_correct,
    next_review_at,
    last_answered_at
)
VALUES (
    $1,
    $2,
    1,
    $3,
    $4,
    $5,
    CASE WHEN $6::BOOLEAN AND $7::INTEGER = 1 THEN 1 ELSE 0 END,
    CASE WHEN $6::BOOLEAN THEN 1 ELSE 0 END,
    CASE
        WHEN NOT $6::BOOLEAN THEN now() + INTERVAL '10 minutes'
        ELSE now() + INTERVAL '1 day'
    END,
    now()
)
ON CONFLICT (user_id, question_id)
DO UPDATE SET
    completed_count = progress.completed_count + 1,
    correct_count = progress.correct_count + EXCLUDED.correct_count,
    failed_count = progress.failed_count + EXCLUDED.failed_count,
    first_try_correct_count =
        progress.first_try_correct_count + EXCLUDED.first_try_correct_count,
    review_stage = CASE
        WHEN NOT $6::BOOLEAN THEN 0
        WHEN $7::INTEGER = 1 THEN LEAST(progress.review_stage + 1, 5)
        ELSE progress.review_stage
    END,
    consecutive_correct = CASE
        WHEN $6::BOOLEAN THEN progress.consecutive_correct + 1
        ELSE 0
    END,
    next_review_at = CASE
        WHEN NOT $6::BOOLEAN THEN now() + INTERVAL '10 minutes'
        WHEN $7::INTEGER <> 1 THEN now() + INTERVAL '1 day'
        WHEN progress.review_stage + 1 <= 1 THEN now() + INTERVAL '1 day'
        WHEN progress.review_stage + 1 = 2 THEN now() + INTERVAL '3 days'
        WHEN progress.review_stage + 1 = 3 THEN now() + INTERVAL '7 days'
        WHEN progress.review_stage + 1 = 4 THEN now() + INTERVAL '14 days'
        ELSE now() + INTERVAL '30 days'
    END,
    last_answered_at = now();
)sql",
                    {
                        user_id,
                        std::string(submission.question_id),
                        correct_increment,
                        failed_increment,
                        first_try_increment,
                        correct,
                        attempt_number,
                    },
                    false);

      ExecuteParams(impl_->connection,
                    R"sql(
INSERT INTO topic_progress AS progress(
    user_id,
    topic,
    adaptive_level,
    adaptive_evidence,
    completed_count,
    correct_count,
    failed_count,
    first_try_correct_count,
    last_answered_at
)
VALUES ($1, $2, $3, $4, 1, $5, $6, $7, now())
ON CONFLICT (user_id, topic)
DO UPDATE SET
    adaptive_level = EXCLUDED.adaptive_level,
    adaptive_evidence = EXCLUDED.adaptive_evidence,
    completed_count = progress.completed_count + 1,
    correct_count = progress.correct_count + EXCLUDED.correct_count,
    failed_count = progress.failed_count + EXCLUDED.failed_count,
    first_try_correct_count =
        progress.first_try_correct_count + EXCLUDED.first_try_correct_count,
    last_answered_at = now();
)sql",
                    {
                        user_id,
                        std::string(submission.topic),
                        std::to_string(submission.topic_level),
                        std::to_string(submission.topic_level_evidence),
                        correct_increment,
                        failed_increment,
                        first_try_increment,
                    },
                    false);

      ExecuteParams(impl_->connection,
                    R"sql(
UPDATE users
SET adaptive_level = $2,
    adaptive_evidence = $3,
    unlocked_level = GREATEST(unlocked_level, $2::INTEGER)
WHERE user_id = $1;
)sql",
                    {
                        user_id,
                        std::to_string(submission.adaptive_level),
                        std::to_string(submission.adaptive_level_evidence),
                    },
                    false);
    }

    Execute(impl_->connection, "COMMIT");
    return RecordSubmissionResult::kInserted;
  } catch (...) {
    try {
      Execute(impl_->connection, "ROLLBACK");
    } catch (...) {}

    throw;
  }
}

void AttemptRepository::FinishSession(std::int64_t session_id, std::string_view finish_reason) {
  if (finish_reason.empty()) {
    throw std::invalid_argument("finish reason must not be empty");
  }

  ExecuteParams(impl_->connection,
                R"sql(
UPDATE sessions
SET finished_at = now(),
    finish_reason = $2
WHERE id = $1
  AND finished_at IS NULL;
)sql",
                {
                    std::to_string(session_id),
                    std::string(finish_reason),
                },
                false);
}

void AttemptRepository::FinishOpenSessions(std::int64_t user_id, std::string_view finish_reason) {
  if (finish_reason.empty()) {
    throw std::invalid_argument("finish reason must not be empty");
  }

  ExecuteParams(impl_->connection,
                R"sql(
UPDATE sessions
SET finished_at = now(),
    finish_reason = $2
WHERE user_id = $1
  AND finished_at IS NULL;
)sql",
                {
                    std::to_string(user_id),
                    std::string(finish_reason),
                },
                false);
}

void AttemptRepository::FinishOpenSessionsWithModePrefix(std::int64_t user_id, std::string_view mode_prefix,
                                                         std::string_view finish_reason) {
  if (mode_prefix.empty()) {
    throw std::invalid_argument("mode prefix must not be empty");
  }
  if (finish_reason.empty()) {
    throw std::invalid_argument("finish reason must not be empty");
  }

  ExecuteParams(impl_->connection,
                R"sql(
UPDATE sessions
SET finished_at = now(),
    finish_reason = $3
WHERE user_id = $1
  AND left(mode, char_length($2)) = $2
  AND finished_at IS NULL;
)sql",
                {
                    std::to_string(user_id),
                    std::string(mode_prefix),
                    std::string(finish_reason),
                },
                false);
}
