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

enum class ReconnectPolicy : std::uint8_t {
  kAllow,
  kForbid,
};

void EnsureConnection(PGconn* connection, ReconnectPolicy reconnect_policy = ReconnectPolicy::kAllow) {
  if (connection == nullptr) {
    ThrowDatabaseError(connection, "PostgreSQL connection is not initialized");
  }

  if (PQstatus(connection) == CONNECTION_OK) {
    return;
  }

  if (reconnect_policy == ReconnectPolicy::kForbid) {
    ThrowDatabaseError(connection, "PostgreSQL connection was lost during a transaction");
  }

  PQreset(connection);
  if (PQstatus(connection) != CONNECTION_OK) {
    ThrowDatabaseError(connection, "failed to reconnect to PostgreSQL");
  }
}

void Execute(PGconn* connection, std::string_view sql, ReconnectPolicy reconnect_policy = ReconnectPolicy::kAllow) {
  EnsureConnection(connection, reconnect_policy);
  const std::string sql_text(sql);
  Result result(PQexec(connection, sql_text.c_str()));

  CheckCommand(connection, result, "PostgreSQL command failed");
}

Result ExecuteParams(PGconn* connection, const char* sql, const std::vector<std::string>& params, bool expect_rows,
                     ReconnectPolicy reconnect_policy = ReconnectPolicy::kAllow) {
  EnsureConnection(connection, reconnect_policy);
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

class Transaction {
 public:
  explicit Transaction(PGconn* connection) : connection_(connection) {
    ::Execute(connection_, "BEGIN");
    active_ = true;
  }

  ~Transaction() { Rollback(); }

  Transaction(const Transaction&) = delete;
  Transaction& operator=(const Transaction&) = delete;

  void Execute(std::string_view sql) { ::Execute(connection_, sql, ReconnectPolicy::kForbid); }

  [[nodiscard]]
  Result ExecuteParams(const char* sql, const std::vector<std::string>& params, bool expect_rows) {
    return ::ExecuteParams(connection_, sql, params, expect_rows, ReconnectPolicy::kForbid);
  }

  void Commit() {
    Execute("COMMIT");
    active_ = false;
  }

 private:
  void Rollback() noexcept {
    if (!active_ || connection_ == nullptr || PQstatus(connection_) != CONNECTION_OK) {
      return;
    }

    PGresult* result = PQexec(connection_, "ROLLBACK");
    if (result != nullptr) {
      PQclear(result);
    }
    active_ = false;
  }

  PGconn* connection_ = nullptr;
  bool active_ = false;
};

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
    Transaction transaction(connection);
    static_cast<void>(transaction.ExecuteParams("SELECT pg_advisory_xact_lock(1936028748);", {}, true));

    const auto applied = transaction.ExecuteParams("SELECT EXISTS(SELECT 1 FROM schema_migrations WHERE version = $1);",
                                                   {version}, true);

    if (PQntuples(applied.Get()) != 1 || PQnfields(applied.Get()) != 1) {
      throw std::runtime_error("unexpected migration lookup result");
    }
    if (ParseBool(PQgetvalue(applied.Get(), 0, 0))) {
      transaction.Commit();
      continue;
    }

    transaction.Execute(ReadFile(migration));
    static_cast<void>(
        transaction.ExecuteParams("INSERT INTO schema_migrations(version) VALUES ($1);", {version}, false));
    transaction.Commit();
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

std::unordered_set<std::string> AttemptRepository::LoadCleanQuestionFamilies(std::int64_t user_id) {
  EnsureUser(user_id);

  const auto result = ExecuteParams(impl_->connection,
                                    R"sql(
SELECT question_family_id
FROM skill_evidence
WHERE user_id = $1
  AND clean_success = true;
)sql",
                                    {
                                        std::to_string(user_id),
                                    },
                                    true);

  std::unordered_set<std::string> families;
  const auto row_count = PQntuples(result.Get());
  families.reserve(static_cast<std::size_t>(row_count));
  for (int row = 0; row < row_count; ++row) {
    const auto* family_id = PQgetvalue(result.Get(), row, 0);
    if (family_id == nullptr || *family_id == '\0') {
      throw std::runtime_error("stored question family id must not be empty");
    }
    families.emplace(family_id);
  }
  return families;
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
    ),
    (
        SELECT COALESCE(SUM(delta), 0)
        FROM xp_ledger
        WHERE xp_ledger.user_id = users.user_id
    )
FROM users
WHERE users.user_id = $1;
)sql",
                                    {user_id_text}, true);

  if (PQntuples(totals.Get()) != 1 || PQnfields(totals.Get()) != 12) {
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
      .xp_total = ParseInt64(PQgetvalue(totals.Get(), 0, 11)),
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
  ValidateProgressCount(summary.xp_total, "XP total");

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
WITH progress AS (
    SELECT
        skill_id,
        COUNT(*) AS questions_seen,
        COALESCE(SUM(completed_count), 0) AS completed_count,
        COALESCE(SUM(correct_count), 0) AS correct_count,
        COALESCE(SUM(failed_count), 0) AS failed_count,
        COALESCE(SUM(first_try_correct_count), 0) AS first_try_correct_count
    FROM skill_progress
    WHERE user_id = $1
    GROUP BY skill_id
)
SELECT
    progress.skill_id,
    progress.questions_seen,
    progress.completed_count,
    progress.correct_count,
    progress.failed_count,
    progress.first_try_correct_count,
    COALESCE(state.status, 'unassessed'),
    COALESCE(state.successful_family_count, 0),
    COALESCE(state.clean_family_count, 0),
    COALESCE(state.application_family_count, 0),
    COALESCE(state.transfer_family_count, 0)
FROM progress
LEFT JOIN user_skill_state AS state
  ON state.user_id = $1
 AND state.skill_id = progress.skill_id
ORDER BY progress.questions_seen DESC, progress.skill_id;
)sql",
                                    {user_id_text}, true);

  if (PQnfields(skills.Get()) != 11) {
    throw std::runtime_error("unexpected PostgreSQL result when reading skill progress");
  }

  const auto skill_count = PQntuples(skills.Get());
  summary.skills.reserve(static_cast<std::size_t>(skill_count));

  for (int row = 0; row < skill_count; ++row) {
    const auto* skill_id = PQgetvalue(skills.Get(), row, 0);
    if (skill_id == nullptr || *skill_id == '\0') {
      throw std::runtime_error("stored skill id must not be empty");
    }

    SkillProgressSummary skill_summary{
        .skill_id = skill_id,
        .mastery_status = ParseSkillMasteryStatus(PQgetvalue(skills.Get(), row, 6)),
        .successful_family_count = ParseInt64(PQgetvalue(skills.Get(), row, 7)),
        .clean_family_count = ParseInt64(PQgetvalue(skills.Get(), row, 8)),
        .application_family_count = ParseInt64(PQgetvalue(skills.Get(), row, 9)),
        .transfer_family_count = ParseInt64(PQgetvalue(skills.Get(), row, 10)),
        .questions_seen = ParseInt64(PQgetvalue(skills.Get(), row, 1)),
        .completed_count = ParseInt64(PQgetvalue(skills.Get(), row, 2)),
        .correct_count = ParseInt64(PQgetvalue(skills.Get(), row, 3)),
        .failed_count = ParseInt64(PQgetvalue(skills.Get(), row, 4)),
        .first_try_correct_count = ParseInt64(PQgetvalue(skills.Get(), row, 5)),
    };
    ValidateProgressCount(skill_summary.questions_seen, "skill questions seen");
    ValidateProgressCount(skill_summary.successful_family_count, "skill successful family count");
    ValidateProgressCount(skill_summary.clean_family_count, "skill clean family count");
    ValidateProgressCount(skill_summary.application_family_count, "skill application family count");
    ValidateProgressCount(skill_summary.transfer_family_count, "skill transfer family count");
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

RecordSubmissionOutcome AttemptRepository::RecordSubmission(const SubmissionRecord& submission) {
  if (submission.user_id == 0 || submission.session_id == 0) {
    throw std::invalid_argument("user and session ids must not be zero");
  }

  if (submission.question_id.empty() || submission.skill_id.empty() || submission.question_family_id.empty() ||
      submission.answer.empty()) {
    throw std::invalid_argument("question id, skill id, question family id and answer must not be empty");
  }

  if (submission.attempt_number < 1 || submission.attempt_number > 3) {
    throw std::invalid_argument("attempt number must be between 1 and 3");
  }

  if (submission.completed && submission.topic.empty()) {
    throw std::invalid_argument("completed submission topic must not be empty");
  }

  if (submission.transfer && submission.evidence_type != QuestionEvidenceType::kApplication) {
    throw std::invalid_argument("transfer evidence must be an application");
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
  const auto evidence_type = std::string(QuestionEvidenceTypeName(submission.evidence_type));
  const auto transfer = submission.transfer ? "true" : "false";

  Transaction transaction(impl_->connection);

  {
    static_cast<void>(transaction.ExecuteParams(
        R"sql(
INSERT INTO users(user_id)
VALUES ($1)
ON CONFLICT (user_id) DO NOTHING;
)sql",
        {user_id}, false));

    const auto user_lock = transaction.ExecuteParams(
        R"sql(
SELECT user_id
FROM users
WHERE user_id = $1
FOR UPDATE;
)sql",
        {user_id}, true);
    if (PQntuples(user_lock.Get()) != 1 || PQnfields(user_lock.Get()) != 1) {
      throw std::runtime_error("unexpected PostgreSQL result when locking user progress");
    }

    const auto context = transaction.ExecuteParams(
        R"sql(
SELECT
    EXISTS(
        SELECT 1
        FROM attempts
        WHERE user_id = $1
          AND question_id = $2
    ),
    COALESCE((
        SELECT next_review_at <= now()
        FROM question_progress
        WHERE user_id = $1
          AND question_id = $2
    ), false),
    COALESCE((
        SELECT EXTRACT(EPOCH FROM next_review_at)::BIGINT::TEXT
        FROM question_progress
        WHERE user_id = $1
          AND question_id = $2
    ), ''),
    COALESCE((
        SELECT status
        FROM user_skill_state
        WHERE user_id = $1
          AND skill_id = $3
    ), 'unassessed'),
    EXISTS(
        SELECT 1
        FROM user_skill_state
        WHERE user_id = $1
          AND skill_id = $3
          AND demonstrated_at <= now() - INTERVAL '1 day'
    ),
    EXISTS(
        SELECT 1
        FROM skill_evidence
        WHERE user_id = $1
          AND skill_id = $3
          AND question_family_id = $4
    );
)sql",
        {
            user_id,
            std::string(submission.question_id),
            std::string(submission.skill_id),
            std::string(submission.question_family_id),
        },
        true);

    if (PQntuples(context.Get()) != 1 || PQnfields(context.Get()) != 6) {
      throw std::runtime_error("unexpected PostgreSQL result when reading skill evidence context");
    }

    const bool question_seen_before = ParseBool(PQgetvalue(context.Get(), 0, 0));
    const bool review_was_due = ParseBool(PQgetvalue(context.Get(), 0, 1));
    const std::string review_cycle = PQgetvalue(context.Get(), 0, 2);
    const auto previous_mastery = ParseSkillMasteryStatus(PQgetvalue(context.Get(), 0, 3));
    const bool demonstrated_long_enough = ParseBool(PQgetvalue(context.Get(), 0, 4));
    const bool family_seen_before = ParseBool(PQgetvalue(context.Get(), 0, 5));

    const auto attempt = transaction.ExecuteParams(
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
      const auto existing = transaction.ExecuteParams(
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

      transaction.Commit();
      return {
          .result = RecordSubmissionResult::kDuplicate,
          .skill_mastery = std::nullopt,
          .xp_gained = 0,
      };
    }

    static_cast<void>(transaction.ExecuteParams(
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
        false));

    std::optional<SkillMasteryChange> mastery_change;
    std::int64_t xp_gained = 0;

    if (submission.completed) {
      const bool clean_success = submission.correct && submission.attempt_number == 1;
      const bool clean_application = clean_success && submission.evidence_type == QuestionEvidenceType::kApplication;
      const bool delayed_new_application = clean_application && !question_seen_before && demonstrated_long_enough;

      static_cast<void>(transaction.ExecuteParams(
          R"sql(
INSERT INTO skill_evidence AS evidence(
    user_id,
    skill_id,
    question_family_id,
    question_id,
    evidence_type,
    transfer,
    successful,
    clean_success,
    delayed_new_application,
    first_recorded_at,
    last_recorded_at
)
VALUES ($1, $2, $3, $4, $5, $6, $7, $8, $9, now(), now())
ON CONFLICT (user_id, skill_id, question_family_id)
DO UPDATE SET
    question_id = EXCLUDED.question_id,
    successful = evidence.successful OR EXCLUDED.successful,
    clean_success = evidence.clean_success OR EXCLUDED.clean_success,
    delayed_new_application =
        evidence.delayed_new_application OR EXCLUDED.delayed_new_application,
    last_recorded_at = now();
)sql",
          {
              user_id,
              std::string(submission.skill_id),
              std::string(submission.question_family_id),
              std::string(submission.question_id),
              evidence_type,
              transfer,
              submission.correct ? "true" : "false",
              clean_success ? "true" : "false",
              delayed_new_application ? "true" : "false",
          },
          false));

      const auto evidence = transaction.ExecuteParams(
          R"sql(
SELECT
    COUNT(*) FILTER (WHERE successful),
    COUNT(*) FILTER (WHERE clean_success),
    COUNT(*) FILTER (WHERE clean_success AND evidence_type = 'application'),
    COUNT(*) FILTER (WHERE clean_success AND transfer),
    COUNT(*) FILTER (WHERE delayed_new_application)
FROM skill_evidence
WHERE user_id = $1
  AND skill_id = $2;
)sql",
          {
              user_id,
              std::string(submission.skill_id),
          },
          true);

      if (PQntuples(evidence.Get()) != 1 || PQnfields(evidence.Get()) != 5) {
        throw std::runtime_error("unexpected PostgreSQL result when aggregating skill evidence");
      }

      const SkillEvidenceSummary evidence_summary{
          .successful_family_count = ParseInt64(PQgetvalue(evidence.Get(), 0, 0)),
          .clean_family_count = ParseInt64(PQgetvalue(evidence.Get(), 0, 1)),
          .application_family_count = ParseInt64(PQgetvalue(evidence.Get(), 0, 2)),
          .transfer_family_count = ParseInt64(PQgetvalue(evidence.Get(), 0, 3)),
          .delayed_application_count = ParseInt64(PQgetvalue(evidence.Get(), 0, 4)),
      };
      const auto mastery = ApplySkillMastery(previous_mastery, evidence_summary,
                                             SkillMasteryEvent{
                                                 .due_failure = review_was_due && !submission.correct,
                                                 .clean_application = clean_application,
                                             });

      if (mastery.current != SkillMasteryStatus::kUnassessed) {
        static_cast<void>(transaction.ExecuteParams(
            R"sql(
INSERT INTO user_skill_state AS state(
    user_id,
    skill_id,
    status,
    successful_family_count,
    clean_family_count,
    application_family_count,
    transfer_family_count,
    demonstrated_at,
    retained_at,
    needs_recovery_since,
    last_evidence_at
)
VALUES (
    $1, $2, $3, $4, $5, $6, $7,
    CASE WHEN $3 IN ('demonstrated', 'retained') THEN now() ELSE NULL END,
    CASE WHEN $3 = 'retained' THEN now() ELSE NULL END,
    CASE WHEN $3 = 'needs_recovery' THEN now() ELSE NULL END,
    now()
)
ON CONFLICT (user_id, skill_id)
DO UPDATE SET
    status = EXCLUDED.status,
    successful_family_count = EXCLUDED.successful_family_count,
    clean_family_count = EXCLUDED.clean_family_count,
    application_family_count = EXCLUDED.application_family_count,
    transfer_family_count = EXCLUDED.transfer_family_count,
    demonstrated_at = COALESCE(state.demonstrated_at, EXCLUDED.demonstrated_at),
    retained_at = COALESCE(state.retained_at, EXCLUDED.retained_at),
    needs_recovery_since = CASE
        WHEN EXCLUDED.status = 'needs_recovery'
            THEN COALESCE(state.needs_recovery_since, now())
        ELSE NULL
    END,
    last_evidence_at = now();
)sql",
            {
                user_id,
                std::string(submission.skill_id),
                std::string(SkillMasteryStatusName(mastery.current)),
                std::to_string(evidence_summary.successful_family_count),
                std::to_string(evidence_summary.clean_family_count),
                std::to_string(evidence_summary.application_family_count),
                std::to_string(evidence_summary.transfer_family_count),
            },
            false));

        mastery_change = SkillMasteryChange{
            .skill_id = std::string(submission.skill_id),
            .previous = mastery.previous,
            .current = mastery.current,
            .successful_family_count = evidence_summary.successful_family_count,
            .clean_family_count = evidence_summary.clean_family_count,
            .application_family_count = evidence_summary.application_family_count,
            .transfer_family_count = evidence_summary.transfer_family_count,
        };
      }

      const auto award_xp = [&](std::string_view event_type, std::string_view source_id, std::string_view cycle_id,
                                int delta) {
        const auto awarded = transaction.ExecuteParams(
            R"sql(
INSERT INTO xp_ledger(user_id, event_type, source_id, cycle_id, delta, rules_version)
VALUES ($1, $2, $3, $4, $5, 1)
ON CONFLICT (user_id, event_type, source_id, cycle_id) DO NOTHING
RETURNING delta;
)sql",
            {
                user_id,
                std::string(event_type),
                std::string(source_id),
                std::string(cycle_id),
                std::to_string(delta),
            },
            true);
        if (PQnfields(awarded.Get()) != 1 || PQntuples(awarded.Get()) > 1) {
          throw std::runtime_error("unexpected PostgreSQL result when awarding XP");
        }
        if (PQntuples(awarded.Get()) == 1) {
          xp_gained += ParseInt64(PQgetvalue(awarded.Get(), 0, 0));
        }
      };

      if (submission.correct) {
        if (!family_seen_before && submission.attempt_number == 1) {
          award_xp("new_family_first_try", submission.question_family_id, "initial", 12);
        } else if (!family_seen_before && submission.attempt_number == 2) {
          award_xp("new_family_second_try", submission.question_family_id, "initial", 6);
        }

        if (review_was_due && !review_cycle.empty()) {
          award_xp("due_review", submission.question_id, review_cycle, 8);
        }

        if (!family_seen_before && clean_success && submission.transfer) {
          award_xp("transfer_application", submission.question_family_id, "initial", 15);
        }

        if (mastery.current == SkillMasteryStatus::kDemonstrated &&
            mastery.previous != SkillMasteryStatus::kDemonstrated) {
          award_xp("skill_demonstrated", submission.skill_id, "initial", 30);
        }
      }

      static_cast<void>(transaction.ExecuteParams(
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
          false));

      static_cast<void>(transaction.ExecuteParams(
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
          false));

      static_cast<void>(transaction.ExecuteParams(
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
          false));
    }

    transaction.Commit();
    return {
        .result = RecordSubmissionResult::kInserted,
        .skill_mastery = std::move(mastery_change),
        .xp_gained = xp_gained,
    };
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
