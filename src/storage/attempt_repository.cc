#include "attempt_repository.h"

#include "learning/review_schedule.h"
#include "quiz/question_identity.h"
#include "storage/postgres_connection.h"

#include <libpq-fe.h>
#include <boost/json.hpp>

#include <algorithm>
#include <charconv>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <unordered_map>
#include <utility>
#include <vector>

namespace {

constexpr std::size_t kMaximumSessionSnapshotBytes = 1024 * 1024;

void ValidateSessionSnapshot(std::string_view content_version, std::string_view state_json) {
  if (content_version.empty() || content_version.size() > 128) {
    throw std::invalid_argument("active session content version has invalid length");
  }
  if (state_json.empty() || state_json.size() > kMaximumSessionSnapshotBytes) {
    throw std::invalid_argument("active session snapshot has invalid size");
  }
}

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
void ThrowDatabaseError(const PostgresConnection* connection, std::string_view operation) {
  std::string message(operation);
  message += ": ";

  if (connection != nullptr) {
    message += connection->ErrorMessage();
  } else {
    message += "unknown PostgreSQL error";
  }

  throw std::runtime_error(message);
}

void CheckCommand(const PostgresConnection* connection, const Result& result, std::string_view operation) {
  if (PQresultStatus(result.Get()) != PGRES_COMMAND_OK) {
    ThrowDatabaseError(connection, operation);
  }
}

void CheckQuery(const PostgresConnection* connection, const Result& result, std::string_view operation) {
  if (PQresultStatus(result.Get()) != PGRES_TUPLES_OK) {
    ThrowDatabaseError(connection, operation);
  }
}

enum class ReconnectPolicy : std::uint8_t {
  kAllow,
  kForbid,
};

void Execute(PostgresConnection* connection, std::string_view sql,
             ReconnectPolicy reconnect_policy = ReconnectPolicy::kAllow) {
  const std::string sql_text(sql);
  Result result(connection->Execute(sql_text.c_str(), nullptr, reconnect_policy == ReconnectPolicy::kAllow));

  CheckCommand(connection, result, "PostgreSQL command failed");
}

Result ExecuteParams(PostgresConnection* connection, const char* sql, const std::vector<std::string>& params,
                     bool expect_rows, ReconnectPolicy reconnect_policy = ReconnectPolicy::kAllow) {
  Result result(connection->Execute(sql, &params, reconnect_policy == ReconnectPolicy::kAllow));

  if (expect_rows) {
    CheckQuery(connection, result, "PostgreSQL query failed");
  } else {
    CheckCommand(connection, result, "PostgreSQL command failed");
  }

  return result;
}

class Transaction {
 public:
  explicit Transaction(PostgresConnection* connection) : connection_(connection) {
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
    if (!active_ || connection_ == nullptr) {
      return;
    }

    connection_->Rollback();
    active_ = false;
  }

  PostgresConnection* connection_ = nullptr;
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

std::int64_t UpdateActiveSessionSnapshot(Transaction& transaction, std::int64_t user_id, std::int64_t session_id,
                                         std::int64_t expected_version, std::string_view content_version,
                                         std::string_view state_json) {
  if (user_id == 0 || session_id == 0 || expected_version <= 0) {
    throw std::invalid_argument("active session identity and version must be positive");
  }
  ValidateSessionSnapshot(content_version, state_json);

  const auto result = transaction.ExecuteParams(
      R"sql(
UPDATE active_training_sessions
SET state_json = $5::JSONB,
    state_version = state_version + 1,
    content_version = $4,
    updated_at = now(),
    expires_at = 'infinity'
WHERE user_id = $1
  AND session_id = $2
  AND state_version = $3
  AND content_version = $4
  AND expires_at > now()
RETURNING state_version;
)sql",
      {
          std::to_string(user_id),
          std::to_string(session_id),
          std::to_string(expected_version),
          std::string(content_version),
          std::string(state_json),
      },
      true);
  if (PQnfields(result.Get()) != 1 || PQntuples(result.Get()) != 1) {
    throw std::logic_error("active Mini App session changed, expired or is no longer available");
  }
  return ParseInt64(PQgetvalue(result.Get(), 0, 0));
}

void ValidateProgressCount(std::int64_t value, std::string_view name) {
  if (value < 0) {
    throw std::runtime_error("stored " + std::string(name) + " must not be negative");
  }
}

std::string JoinValues(const std::vector<std::string>& values) {
  std::string result;
  for (const auto& value : values) {
    if (value.contains(',')) {
      throw std::invalid_argument("stored preset values must not contain commas");
    }
    if (!result.empty()) {
      result.push_back(',');
    }
    result += value;
  }
  return result;
}

std::vector<std::string> SplitValues(std::string_view value) {
  std::vector<std::string> result;
  while (!value.empty()) {
    const auto delimiter = value.find(',');
    const auto item = value.substr(0, delimiter);
    if (!item.empty()) {
      result.emplace_back(item);
    }
    if (delimiter == std::string_view::npos) {
      break;
    }
    value.remove_prefix(delimiter + 1);
  }
  return result;
}

std::vector<std::string> GrantQualifiedRewards(Transaction& transaction, std::string_view user_id, bool announced) {
  const auto result = transaction.ExecuteParams(
      R"sql(
INSERT INTO user_reward_unlock(user_id, reward_id, unlocked_at, announced_at)
SELECT
    $1,
    reward.reward_id,
    now(),
    CASE WHEN $2::BOOLEAN THEN now() ELSE NULL END
FROM (VALUES
    ('profile_frames', 150),
    ('second_preset', 400)
) AS reward(reward_id, threshold)
WHERE reward.threshold <= COALESCE((
    SELECT SUM(delta)
    FROM xp_ledger
    WHERE user_id = $1
), 0)
ON CONFLICT (user_id, reward_id) DO NOTHING
RETURNING reward_id;
)sql",
      {std::string(user_id), announced ? "true" : "false"}, true);

  if (PQnfields(result.Get()) != 1) {
    throw std::runtime_error("unexpected PostgreSQL result when unlocking rewards");
  }
  std::vector<std::string> unlocked;
  unlocked.reserve(static_cast<std::size_t>(PQntuples(result.Get())));
  for (int row = 0; row < PQntuples(result.Get()); ++row) {
    unlocked.emplace_back(PQgetvalue(result.Get(), row, 0));
  }
  return unlocked;
}

std::string ReadFile(const std::filesystem::path& path) {
  std::ifstream input(path, std::ios::binary);
  if (!input) {
    throw std::runtime_error("failed to open migration: " + path.string());
  }

  return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

void ApplyMigrations(PostgresConnection* connection, const std::filesystem::path& directory) {
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

// Callers hold the user row lock. Keep the due timestamp / XP cycle unchanged:
// this is a failed attempt, not a completed review or a new review cycle.
std::optional<SkillMasteryChange> RecordReviewFailure(Transaction& transaction, std::int64_t user_id,
                                                      const ReviewFailureContext& failure) {
  const std::vector<std::string> params{std::to_string(user_id), std::string(failure.question_id),
                                        std::string(failure.skill_id)};
  const auto previous = transaction.ExecuteParams(
      R"sql(
SELECT status, successful_family_count, clean_family_count, application_family_count, transfer_family_count
FROM user_skill_state
WHERE user_id = $1 AND skill_id = $3
  AND status IN ('demonstrated', 'retained', 'needs_recovery')
  AND EXISTS (
    SELECT 1 FROM question_progress
    WHERE user_id = $1 AND question_id = $2 AND next_review_at <= now()
  );
)sql",
      params, true);

  static_cast<void>(transaction.ExecuteParams(
      R"sql(
UPDATE question_progress
SET review_stage = LEAST(review_stage, $3::INTEGER), consecutive_correct = 0
WHERE user_id = $1 AND question_id = $2;
)sql",
      {params[0], params[1], std::to_string(kMaximumRepairReviewStage)}, false));

  if (PQntuples(previous.Get()) == 0) {
    return std::nullopt;
  }
  if (PQntuples(previous.Get()) != 1 || PQnfields(previous.Get()) != 5) {
    throw std::runtime_error("unexpected PostgreSQL result when recording review failure");
  }
  const SkillEvidenceSummary evidence{
      .successful_family_count = ParseInt64(PQgetvalue(previous.Get(), 0, 1)),
      .clean_family_count = ParseInt64(PQgetvalue(previous.Get(), 0, 2)),
      .application_family_count = ParseInt64(PQgetvalue(previous.Get(), 0, 3)),
      .transfer_family_count = ParseInt64(PQgetvalue(previous.Get(), 0, 4)),
  };
  const auto mastery = ApplySkillMastery(ParseSkillMasteryStatus(PQgetvalue(previous.Get(), 0, 0)), evidence,
                                         SkillMasteryEvent{.due_failure = true});
  static_cast<void>(transaction.ExecuteParams(
      R"sql(
UPDATE user_skill_state
SET status = $3, needs_recovery_since = COALESCE(needs_recovery_since, now())
WHERE user_id = $1 AND skill_id = $2;
)sql",
      {params[0], params[2], std::string(SkillMasteryStatusName(mastery.current))}, false));
  return SkillMasteryChange{
      .skill_id = std::string(failure.skill_id),
      .previous = mastery.previous,
      .current = mastery.current,
      .successful_family_count = evidence.successful_family_count,
      .clean_family_count = evidence.clean_family_count,
      .application_family_count = evidence.application_family_count,
      .transfer_family_count = evidence.transfer_family_count,
  };
}

}  // namespace

struct DatabaseConnectionPool::Impl {
  using Connection = std::unique_ptr<PostgresConnection>;

  class Lease {
   public:
    Lease(Impl& owner, std::size_t index) : owner_(&owner), index_(index) {}
    ~Lease() {
      if (owner_ != nullptr) {
        owner_->Release(index_);
      }
    }

    Lease(const Lease&) = delete;
    Lease& operator=(const Lease&) = delete;
    Lease(Lease&& other) noexcept
        : owner_(std::exchange(other.owner_, nullptr)), index_(std::exchange(other.index_, 0)) {}
    Lease& operator=(Lease&&) = delete;

    [[nodiscard]] PostgresConnection* Get() const noexcept { return owner_->connections[index_].get(); }

   private:
    Impl* owner_ = nullptr;
    std::size_t index_ = 0;
  };

  Impl(const std::string& database_url, std::size_t connection_count, DatabaseTimeouts timeout_policy)
      : timeouts(timeout_policy) {
    if (connection_count == 0 || connection_count > 32) {
      throw std::invalid_argument("database connection pool size must be between 1 and 32");
    }
    connections.reserve(connection_count);
    available.reserve(connection_count);
    for (std::size_t index = 0; index < connection_count; ++index) {
      auto connection = std::make_unique<PostgresConnection>(database_url, timeouts, shutdown.get_token());
      connections.push_back(std::move(connection));
      available.push_back(index);
    }
    connections.front()->SetMigrationMode(true);
    ApplyMigrations(connections.front().get(), "migrations");
    connections.front()->SetMigrationMode(false);
  }

  [[nodiscard]] Lease Acquire(std::chrono::milliseconds timeout = std::chrono::milliseconds::zero()) {
    std::unique_lock lock(mutex);
    if (!available_condition.wait_for(lock, shutdown.get_token(), timeout.count() > 0 ? timeout : timeouts.acquire,
                                      [this] { return !available.empty(); }) ||
        shutdown.stop_requested()) {
      throw std::runtime_error("database connection pool is busy");
    }
    const auto index = available.back();
    available.pop_back();
    return Lease(*this, index);
  }

  void Release(std::size_t index) {
    {
      std::scoped_lock lock(mutex);
      available.push_back(index);
    }
    available_condition.notify_one();
  }

  std::vector<Connection> connections;
  std::vector<std::size_t> available;
  std::mutex mutex;
  std::condition_variable_any available_condition;
  std::stop_source shutdown;
  DatabaseTimeouts timeouts;
};

DatabaseConnectionPool::DatabaseConnectionPool(std::string database_url, std::size_t connection_count,
                                               DatabaseTimeouts timeouts)
    : impl_(std::make_unique<Impl>(database_url, connection_count, timeouts)) {}

DatabaseConnectionPool::~DatabaseConnectionPool() = default;

std::size_t DatabaseConnectionPool::Size() const noexcept {
  return impl_->connections.size();
}

std::size_t DatabaseConnectionPool::Available() const {
  std::scoped_lock lock(impl_->mutex);
  return impl_->available.size();
}

void DatabaseConnectionPool::RequestStop() noexcept {
  impl_->shutdown.request_stop();
  impl_->available_condition.notify_all();
}

struct AttemptRepository::Impl {
  explicit Impl(std::shared_ptr<DatabaseConnectionPool> connection_pool) : pool(std::move(connection_pool)) {
    if (!pool) {
      throw std::invalid_argument("database connection pool must not be null");
    }
  }

  [[nodiscard]] auto Acquire() { return pool->impl_->Acquire(); }

  std::shared_ptr<DatabaseConnectionPool> pool;
};

AttemptRepository::AttemptRepository(std::string database_url)
    : AttemptRepository(std::make_shared<DatabaseConnectionPool>(std::move(database_url), 1)) {}

AttemptRepository::AttemptRepository(std::shared_ptr<DatabaseConnectionPool> connection_pool)
    : impl_(std::make_unique<Impl>(std::move(connection_pool))) {}

AttemptRepository::~AttemptRepository() = default;

AttemptRepository::AttemptRepository(AttemptRepository&&) noexcept = default;

AttemptRepository& AttemptRepository::operator=(AttemptRepository&&) noexcept = default;

namespace {

constexpr const char* kFeedbackColumns =
    "id, user_id, session_id, request_id, question_id, public_id, content_revision, content_snapshot::TEXT, "
    "phase, category, comment, status, decision, source, state_version, "
    "to_char(created_at AT TIME ZONE 'UTC', 'YYYY-MM-DD\"T\"HH24:MI:SS.MS\"Z\"'), "
    "to_char(updated_at AT TIME ZONE 'UTC', 'YYYY-MM-DD\"T\"HH24:MI:SS.MS\"Z\"'), reference_context::TEXT";

CardFeedbackRecord ReadCardFeedback(const Result& result, int row) {
  const auto text = [&](int column) {
    return std::string(PQgetvalue(result.Get(), row, column));
  };
  return {
      .id = ParseInt64(PQgetvalue(result.Get(), row, 0)),
      .submission =
          {
              .user_id = ParseInt64(PQgetvalue(result.Get(), row, 1)),
              .session_id = ParseInt64(PQgetvalue(result.Get(), row, 2)),
              .request_id = text(3),
              .question_id = text(4),
              .public_id = text(5),
              .content_revision = text(6),
              .content_snapshot_json = text(7),
              .phase = text(8),
              .category = text(9),
              .comment = text(10),
          },
      .status = text(11),
      .decision = text(12),
      .source = text(13),
      .version = ParseInt64(PQgetvalue(result.Get(), row, 14)),
      .created_at = text(15),
      .updated_at = text(16),
      .reference_context_json = text(17),
  };
}

bool FeedbackStatusValid(std::string_view status) {
  return status == "new" || status == "in_review" || status == "resolved" || status == "rejected";
}

void ValidateFeedbackText(std::string_view text, std::size_t max_bytes, bool allow_empty = false) {
  if ((!allow_empty && text.find_first_not_of(" \t\r\n") == std::string_view::npos) || text.size() > max_bytes ||
      text.find('\0') != std::string_view::npos) {
    throw std::invalid_argument("feedback text has invalid length or characters");
  }
}

}  // namespace

void AttemptRepository::ArchiveQuestionContent(const std::vector<const Question*>& questions) {
  auto connection = impl_->Acquire();
  Transaction transaction(connection.Get());
  for (const auto* question : questions) {
    if (question == nullptr || question->content_revision.size() != 64 || question->content_snapshot_json.empty()) {
      throw std::invalid_argument("cannot archive a question without content identity");
    }
    const auto result =
        transaction.ExecuteParams(R"sql(
INSERT INTO question_content_revisions(question_id, content_revision, content_snapshot)
VALUES ($1, $2, $3::JSONB)
ON CONFLICT (question_id, content_revision) DO UPDATE SET content_revision = EXCLUDED.content_revision
WHERE question_content_revisions.content_snapshot = EXCLUDED.content_snapshot
RETURNING question_id;
)sql",
                                  {question->id, question->content_revision, question->content_snapshot_json}, true);
    if (PQntuples(result.Get()) != 1) {
      throw std::logic_error("a question revision already refers to different content");
    }
  }
  transaction.Commit();
}

std::optional<std::string> AttemptRepository::LoadQuestionContent(std::string_view question_id,
                                                                  std::string_view revision) {
  auto connection = impl_->Acquire();
  const auto result = ExecuteParams(connection.Get(), R"sql(
SELECT content_snapshot::TEXT FROM question_content_revisions WHERE question_id = $1 AND content_revision = $2;
)sql",
                                    {std::string(question_id), std::string(revision)}, true);
  if (PQntuples(result.Get()) == 0) {
    return std::nullopt;
  }
  return std::string(PQgetvalue(result.Get(), 0, 0));
}

std::optional<CardFeedbackRecord> AttemptRepository::FindCardFeedback(std::int64_t user_id,
                                                                      std::string_view request_id) {
  ValidateFeedbackText(request_id, 36);
  auto connection = impl_->Acquire();
  const std::string sql =
      std::string("SELECT ") + kFeedbackColumns + " FROM card_feedback WHERE user_id = $1 AND request_id = $2;";
  const auto result =
      ExecuteParams(connection.Get(), sql.c_str(), {std::to_string(user_id), std::string(request_id)}, true);
  if (PQntuples(result.Get()) == 0) {
    return std::nullopt;
  }
  return ReadCardFeedback(result, 0);
}

CardFeedbackRecord AttemptRepository::SaveCardFeedback(const CardFeedbackSubmission& submission) {
  ValidateFeedbackText(submission.request_id, 36);
  ValidateFeedbackText(submission.question_id, 256);
  ValidateFeedbackText(submission.comment, 8000);
  ValidateFeedbackText(submission.content_snapshot_json, 65536);
  if (submission.request_id.size() != 36 || submission.content_revision.size() != 64 ||
      (submission.phase != "question" && submission.phase != "feedback") ||
      (submission.category != "technical_error" && submission.category != "unclear_wording" &&
       submission.category != "answer_or_source" && submission.category != "other")) {
    throw std::invalid_argument("invalid card feedback metadata");
  }
  auto connection = impl_->Acquire();
  Transaction transaction(connection.Get());
  // Serialize submissions by the same user, including concurrent retries and the rate limit.
  static_cast<void>(transaction.ExecuteParams("SELECT user_id FROM users WHERE user_id = $1 FOR UPDATE;",
                                              {std::to_string(submission.user_id)}, true));
  const std::string duplicate_sql =
      std::string("SELECT ") + kFeedbackColumns + " FROM card_feedback WHERE user_id = $1 AND request_id = $2;";
  const auto duplicate = transaction.ExecuteParams(duplicate_sql.c_str(),
                                                   {std::to_string(submission.user_id), submission.request_id}, true);
  if (PQntuples(duplicate.Get()) != 0) {
    auto existing = ReadCardFeedback(duplicate, 0);
    const auto& first = existing.submission;
    if (first.question_id != submission.question_id || first.public_id != submission.public_id ||
        first.content_revision != submission.content_revision || first.phase != submission.phase ||
        first.category != submission.category || first.comment != submission.comment ||
        first.session_id != submission.session_id) {
      throw std::logic_error("feedback request identifier was already used for a different submission");
    }
    transaction.Commit();
    return existing;
  }
  const auto rate = transaction.ExecuteParams(
      "SELECT count(*) FROM card_feedback WHERE user_id = $1 AND created_at > now() - INTERVAL '1 hour';",
      {std::to_string(submission.user_id)}, true);
  if (ParseInt64(PQgetvalue(rate.Get(), 0, 0)) >= 20) {
    throw FeedbackRateLimitError("Слишком много отзывов за час. Попробуй позже или напиши автору по почте.");
  }
  const std::string insert_sql = R"sql(
INSERT INTO card_feedback(user_id, session_id, request_id, question_id, public_id, content_revision,
                          content_snapshot, phase, category, comment, reference_context)
SELECT $1, id, $3, $4, $5, $6, $7::JSONB, $8, $9, $10,
    COALESCE((SELECT jsonb_agg(jsonb_build_object(
        'view_id', v.id, 'reference_id', v.reference_id,
        'reference_content_revision', v.reference_content_revision,
        'content', r.content_snapshot, 'attempt_number', v.attempt_number, 'opened_at', v.opened_at
    ) ORDER BY v.id)
    FROM reference_views v JOIN reference_content_revisions r
        ON r.reference_id = v.reference_id AND r.content_revision = v.reference_content_revision
    WHERE v.user_id = $1 AND v.session_id = $2 AND v.question_id = $4 AND v.question_content_revision = $6),
    '[]'::JSONB)
FROM sessions WHERE id = $2 AND user_id = $1
RETURNING )sql" + std::string(kFeedbackColumns) +
                                 ";";
  const auto result = transaction.ExecuteParams(
      insert_sql.c_str(),
      {std::to_string(submission.user_id), std::to_string(submission.session_id), submission.request_id,
       submission.question_id, submission.public_id, submission.content_revision, submission.content_snapshot_json,
       submission.phase, submission.category, submission.comment},
      true);
  if (PQntuples(result.Get()) != 1) {
    throw std::logic_error("feedback session does not belong to the user");
  }
  auto record = ReadCardFeedback(result, 0);
  transaction.Commit();
  return record;
}

std::vector<CardFeedbackRecord> AttemptRepository::ListCardFeedback(std::string_view status, std::int64_t before_id) {
  if ((!status.empty() && !FeedbackStatusValid(status)) || before_id < 0) {
    throw std::invalid_argument("invalid feedback queue filter");
  }
  auto connection = impl_->Acquire();
  const std::string sql = std::string("SELECT ") + kFeedbackColumns + R"sql(
 FROM card_feedback WHERE ($1 = '' OR status = $1) AND ($2::BIGINT = 0 OR id < $2::BIGINT)
 ORDER BY id DESC LIMIT 30;
)sql";
  const auto result =
      ExecuteParams(connection.Get(), sql.c_str(), {std::string(status), std::to_string(before_id)}, true);
  std::vector<CardFeedbackRecord> records;
  for (int row = 0; row < PQntuples(result.Get()); ++row) {
    records.push_back(ReadCardFeedback(result, row));
  }
  return records;
}

CardFeedbackRecord AttemptRepository::ReviewCardFeedback(std::int64_t feedback_id, std::int64_t expected_version,
                                                         std::int64_t reviewer_id, std::string_view status,
                                                         std::string_view decision, std::string_view source) {
  ValidateFeedbackText(decision, 8000, true);
  ValidateFeedbackText(source, 2048, true);
  if (feedback_id <= 0 || expected_version <= 0 || reviewer_id <= 0 || !FeedbackStatusValid(status)) {
    throw std::invalid_argument("invalid feedback review metadata");
  }
  if ((status == "resolved" || status == "rejected") &&
      (decision.find_first_not_of(" \t\r\n") == std::string_view::npos || !source.starts_with("https://") ||
       source.size() <= 8 || source.find_first_of(" \t\r\n") != std::string_view::npos)) {
    throw std::invalid_argument("Для закрытия отзыва нужны решение и HTTPS-ссылка на источник проверки.");
  }
  auto connection = impl_->Acquire();
  Transaction transaction(connection.Get());
  const std::string sql = R"sql(
UPDATE card_feedback SET status = $3, decision = $4, source = $5, state_version = state_version + 1, updated_at = now()
WHERE id = $1 AND state_version = $2
RETURNING )sql" + std::string(kFeedbackColumns) +
                          ";";
  const auto result = transaction.ExecuteParams(sql.c_str(),
                                                {std::to_string(feedback_id), std::to_string(expected_version),
                                                 std::string(status), std::string(decision), std::string(source)},
                                                true);
  if (PQntuples(result.Get()) != 1) {
    throw std::logic_error("Отзыв уже изменён или не найден. Обнови очередь.");
  }
  auto record = ReadCardFeedback(result, 0);
  static_cast<void>(transaction.ExecuteParams(
      R"sql(
INSERT INTO card_feedback_review_history(feedback_id, reviewer_id, state_version, status, decision, source)
VALUES ($1, $2, $3, $4, $5, $6);
)sql",
      {std::to_string(feedback_id), std::to_string(reviewer_id), std::to_string(record.version), std::string(status),
       std::string(decision), std::string(source)},
      false));
  transaction.Commit();
  return record;
}

void AttemptRepository::EnsureUser(std::int64_t user_id) {
  auto connection = impl_->Acquire();
  ExecuteParams(connection.Get(),
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

bool AttemptRepository::CheckReady() {
  try {
    auto lease = impl_->pool->impl_->Acquire(std::chrono::milliseconds(250));
    auto* connection = lease.Get();
    Result result(connection->Execute(
        "SELECT EXISTS (SELECT 1 FROM schema_migrations WHERE version = '016_training_completion_receipts.sql');",
        nullptr, true, std::chrono::seconds(1)));
    const bool ready = PQresultStatus(result.Get()) == PGRES_TUPLES_OK && PQntuples(result.Get()) == 1 &&
                       ParseBool(PQgetvalue(result.Get(), 0, 0));
    return ready;
  } catch (const std::exception&) {
    return false;
  }
}

void AttemptRepository::RecordBotStart(std::int64_t user_id, std::int64_t message_id) {
  if (user_id <= 0 || message_id <= 0) {
    return;
  }
  EnsureUser(user_id);
  auto connection = impl_->Acquire();
  static_cast<void>(ExecuteParams(connection.Get(), R"sql(
INSERT INTO learning_events(event_key, event_name, user_id)
VALUES ('bot:' || $1 || ':' || $2, 'bot_start', $1::BIGINT) ON CONFLICT DO NOTHING;
)sql",
                                  {std::to_string(user_id), std::to_string(message_id)}, false));
}

void AttemptRepository::RecordSharedChallengeLaunch(std::int64_t user_id, std::string_view challenge_id,
                                                    std::int64_t auth_date, std::string_view outcome) {
  if (user_id <= 0 || auth_date <= 0 || challenge_id.empty() || challenge_id.size() > 64 ||
      (outcome != "ready" && outcome != "locked" && outcome != "unavailable"))
    throw std::invalid_argument("invalid shared challenge launch");
  auto connection = impl_->Acquire();
  static_cast<void>(ExecuteParams(
      connection.Get(), R"sql(
INSERT INTO shared_challenge_launches(user_id, challenge_id, auth_date, outcome)
VALUES ($1::BIGINT, $2, $3::BIGINT, $4) ON CONFLICT DO NOTHING;
)sql",
      {std::to_string(user_id), std::string(challenge_id), std::to_string(auth_date), std::string(outcome)}, false));
}

std::string AttemptRepository::SharedChallengeStatistics(int days) {
  if (days < 1 || days > 90)
    throw std::invalid_argument("statistics period must be between 1 and 90 days");
  auto connection = impl_->Acquire();
  const auto result = ExecuteParams(connection.Get(), R"sql(
WITH settings AS (
  SELECT (((now() AT TIME ZONE 'UTC')::date - ($1::INTEGER - 1))::timestamp AT TIME ZONE 'UTC') AS since
), launches AS (
  SELECT * FROM shared_challenge_launches WHERE user_id > 0 AND occurred_at >= (SELECT since FROM settings)
), challenges AS (
  SELECT * FROM sessions WHERE user_id > 0 AND mode LIKE 'miniapp:challenge:%'
    AND started_at >= (SELECT since FROM settings)
), followups AS (
  SELECT * FROM sessions WHERE user_id > 0 AND mode LIKE 'miniapp:challenge-followup:%'
    AND started_at >= (SELECT since FROM settings)
)
SELECT jsonb_build_object(
  'opened', (SELECT count(*) FROM launches),
  'blocked', (SELECT count(*) FROM launches WHERE outcome <> 'ready'),
  'started', (SELECT count(*) FROM challenges),
  'answered', (SELECT count(*) FROM challenges s WHERE EXISTS (SELECT 1 FROM attempts a WHERE a.session_id = s.id)),
  'completed', (SELECT count(*) FROM challenges WHERE finish_reason = 'completed'),
  'continued', (SELECT count(*) FROM followups s WHERE EXISTS (SELECT 1 FROM attempts a WHERE a.session_id = s.id))
)::TEXT;
)sql",
                                    {std::to_string(days)}, true);
  return PQgetvalue(result.Get(), 0, 0);
}

bool AttemptRepository::RecordActivity(std::int64_t user_id, std::string_view visit_id, std::int64_t sequence,
                                       std::int64_t session_id, std::string_view question_id, std::string_view context,
                                       std::string_view practice_kind) {
  if (visit_id.size() != 36 || sequence < 0 || sequence > 1'000'000'000 || question_id.size() > 256 ||
      (context != "question" && context != "feedback" && context != "theory" && context != "home" &&
       context != "admin" && context != "hidden" && context != "report")) {
    throw std::invalid_argument("invalid activity pulse");
  }
  EnsureUser(user_id);
  auto connection = impl_->Acquire();
  const auto result = ExecuteParams(
      connection.Get(), R"sql(
SELECT analytics_pulse($1::BIGINT, $2::UUID, $3::BIGINT, NULLIF($4::BIGINT, 0), $5, $6, $7);
)sql",
      {std::to_string(user_id), std::string(visit_id), std::to_string(sequence), std::to_string(session_id),
       std::string(question_id), std::string(context), std::string(practice_kind)},
      true);
  return ParseBool(PQgetvalue(result.Get(), 0, 0));
}

std::string AttemptRepository::BetaStatistics(int days) {
  if (days < 1 || days > 90) {
    throw std::invalid_argument("statistics period must be between 1 and 90 days");
  }
  auto connection = impl_->Acquire();
  const auto result =
      ExecuteParams(connection.Get(),
                    "SELECT (beta_statistics($1::INTEGER) || "
                    "jsonb_build_object('learning', learning_statistics($1::INTEGER), "
                    "'reference_opened', (SELECT count(*) FROM learning_events WHERE user_id > 0 "
                    "AND event_name = 'reference_opened' AND occurred_at >= "
                    "(((now() AT TIME ZONE 'UTC')::date - ($1::INTEGER - 1))::timestamp AT TIME ZONE 'UTC'))))::TEXT;",
                    {std::to_string(days)}, true);
  return PQgetvalue(result.Get(), 0, 0);
}

std::optional<std::int64_t> AttemptRepository::FindSessionByRequest(std::int64_t user_id, std::string_view request_id) {
  auto connection = impl_->Acquire();
  const auto result =
      ExecuteParams(connection.Get(), "SELECT id FROM sessions WHERE user_id = $1 AND client_request_id = $2;",
                    {std::to_string(user_id), std::string(request_id)}, true);
  if (PQntuples(result.Get()) == 0) {
    return std::nullopt;
  }
  return ParseInt64(PQgetvalue(result.Get(), 0, 0));
}

AdaptiveLevelState AttemptRepository::GetAdaptiveState(std::int64_t user_id) {
  EnsureUser(user_id);
  auto connection = impl_->Acquire();

  const auto result = ExecuteParams(connection.Get(),
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
  auto connection = impl_->Acquire();

  const auto result = ExecuteParams(connection.Get(),
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
  auto connection = impl_->Acquire();

  const auto result = ExecuteParams(connection.Get(),
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
  auto connection = impl_->Acquire();

  const auto result = ExecuteParams(connection.Get(),
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
  auto connection = impl_->Acquire();

  const auto result = ExecuteParams(connection.Get(),
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

namespace {
UserProgressSummary ReadProgressSummary(PostgresConnection* connection, std::int64_t user_id,
                                        ReconnectPolicy reconnect_policy) {
  const auto query = [&](const char* sql, const std::vector<std::string>& params, bool rows) {
    return ExecuteParams(connection, sql, params, rows, reconnect_policy);
  };
  const auto user_id_text = std::to_string(user_id);
  const auto totals = query(
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

  const auto topics = query(
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

  const auto skills = query(
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

  // Attempts, unlike completion counters, include errors on abandoned cards and pre-011 history.
  const auto answers = query(R"sql(
SELECT question_id, count(*), count(*) FILTER (WHERE correct),
       count(*) FILTER (WHERE attempt_number = 1),
       count(*) FILTER (WHERE attempt_number = 1 AND correct AND NOT assisted)
FROM attempts WHERE user_id = $1 GROUP BY question_id ORDER BY question_id;
)sql",
                             {user_id_text}, true);
  for (int row = 0; row < PQntuples(answers.Get()); ++row) {
    QuestionAnswerStatistics item{
        .question_id = PQgetvalue(answers.Get(), row, 0),
        .answers = {.attempts = ParseInt64(PQgetvalue(answers.Get(), row, 1)),
                    .correct_attempts = ParseInt64(PQgetvalue(answers.Get(), row, 2)),
                    .first_answers = ParseInt64(PQgetvalue(answers.Get(), row, 3)),
                    .correct_first_answers = ParseInt64(PQgetvalue(answers.Get(), row, 4))},
    };
    summary.answers += item.answers;
    summary.question_answers.push_back(std::move(item));
  }

  return summary;
}
}  // namespace

UserProgressSummary AttemptRepository::LoadProgressSummary(std::int64_t user_id) {
  EnsureUser(user_id);
  auto connection = impl_->Acquire();
  return ReadProgressSummary(connection.Get(), user_id, ReconnectPolicy::kAllow);
}

RewardAccount AttemptRepository::LoadRewardAccount(std::int64_t user_id) {
  EnsureUser(user_id);
  auto connection = impl_->Acquire();
  const auto user_id_text = std::to_string(user_id);
  Transaction transaction(connection.Get());

  static_cast<void>(GrantQualifiedRewards(transaction, user_id_text, false));
  const auto rewards = transaction.ExecuteParams(
      R"sql(
SELECT reward_id, announced_at IS NULL
FROM user_reward_unlock
WHERE user_id = $1
ORDER BY unlocked_at, reward_id
FOR UPDATE;
)sql",
      {user_id_text}, true);
  if (PQnfields(rewards.Get()) != 2) {
    throw std::runtime_error("unexpected PostgreSQL result when reading rewards");
  }

  RewardAccount account;
  for (int row = 0; row < PQntuples(rewards.Get()); ++row) {
    const std::string reward_id = PQgetvalue(rewards.Get(), row, 0);
    account.unlocked_reward_ids.insert(reward_id);
    if (ParseBool(PQgetvalue(rewards.Get(), row, 1))) {
      account.newly_unlocked_reward_ids.push_back(reward_id);
    }
  }
  account.preset_slot_count = account.unlocked_reward_ids.contains("second_preset") ? 2 : 1;

  static_cast<void>(transaction.ExecuteParams(
      R"sql(
INSERT INTO user_reward_settings(user_id)
VALUES ($1)
ON CONFLICT (user_id) DO NOTHING;
)sql",
      {user_id_text}, false));
  const auto settings = transaction.ExecuteParams("SELECT profile_frame FROM user_reward_settings WHERE user_id = $1;",
                                                  {user_id_text}, true);
  if (PQntuples(settings.Get()) != 1 || PQnfields(settings.Get()) != 1) {
    throw std::runtime_error("unexpected PostgreSQL result when reading reward settings");
  }
  account.profile_frame = PQgetvalue(settings.Get(), 0, 0);

  const auto presets = transaction.ExecuteParams(
      R"sql(
SELECT slot, name, modes, topics, level
FROM training_presets
WHERE user_id = $1
ORDER BY slot;
)sql",
      {user_id_text}, true);
  if (PQnfields(presets.Get()) != 5) {
    throw std::runtime_error("unexpected PostgreSQL result when reading training presets");
  }
  account.presets.reserve(static_cast<std::size_t>(PQntuples(presets.Get())));
  for (int row = 0; row < PQntuples(presets.Get()); ++row) {
    account.presets.push_back(TrainingPreset{
        .slot = static_cast<int>(ParseInt64(PQgetvalue(presets.Get(), row, 0))),
        .name = PQgetvalue(presets.Get(), row, 1),
        .modes = SplitValues(PQgetvalue(presets.Get(), row, 2)),
        .topics = SplitValues(PQgetvalue(presets.Get(), row, 3)),
        .level = static_cast<int>(ParseInt64(PQgetvalue(presets.Get(), row, 4))),
    });
  }

  if (!account.newly_unlocked_reward_ids.empty()) {
    static_cast<void>(transaction.ExecuteParams(
        "UPDATE user_reward_unlock SET announced_at = now() WHERE user_id = $1 AND announced_at IS NULL;",
        {user_id_text}, false));
  }
  transaction.Commit();
  return account;
}

void AttemptRepository::SetProfileFrame(std::int64_t user_id, std::string_view frame_id) {
  if (frame_id.empty()) {
    throw std::invalid_argument("profile frame id must not be empty");
  }
  EnsureUser(user_id);
  auto connection = impl_->Acquire();
  const auto result = ExecuteParams(connection.Get(),
                                    R"sql(
INSERT INTO user_reward_settings(user_id, profile_frame, updated_at)
SELECT $1, $2, now()
WHERE $2 = 'none'
   OR EXISTS(
       SELECT 1
       FROM user_reward_unlock
       WHERE user_id = $1
         AND reward_id = 'profile_frames'
   )
ON CONFLICT (user_id)
DO UPDATE SET profile_frame = EXCLUDED.profile_frame, updated_at = now()
RETURNING profile_frame;
)sql",
                                    {std::to_string(user_id), std::string(frame_id)}, true);
  if (PQntuples(result.Get()) != 1 || PQnfields(result.Get()) != 1) {
    throw std::invalid_argument("эта рамка профиля ещё не открыта");
  }
}

void AttemptRepository::SaveTrainingPreset(std::int64_t user_id, const TrainingPreset& preset) {
  if (preset.slot < 1 || preset.slot > 2 || preset.name.empty() || preset.name.size() > 80 || preset.modes.empty() ||
      preset.level < 1 || preset.level > 10) {
    throw std::invalid_argument("invalid training preset");
  }
  EnsureUser(user_id);
  auto connection = impl_->Acquire();
  const auto result = ExecuteParams(connection.Get(),
                                    R"sql(
INSERT INTO training_presets(user_id, slot, name, modes, topics, level, updated_at)
SELECT $1, $2, $3, $4, $5, $6, now()
WHERE $2::INTEGER = 1
   OR EXISTS(
       SELECT 1
       FROM user_reward_unlock
       WHERE user_id = $1
         AND reward_id = 'second_preset'
   )
ON CONFLICT (user_id, slot)
DO UPDATE SET
    name = EXCLUDED.name,
    modes = EXCLUDED.modes,
    topics = EXCLUDED.topics,
    level = EXCLUDED.level,
    updated_at = now()
RETURNING slot;
)sql",
                                    {
                                        std::to_string(user_id),
                                        std::to_string(preset.slot),
                                        preset.name,
                                        JoinValues(preset.modes),
                                        JoinValues(preset.topics),
                                        std::to_string(preset.level),
                                    },
                                    true);
  if (PQntuples(result.Get()) != 1 || PQnfields(result.Get()) != 1) {
    throw std::invalid_argument("этот слот набора ещё не открыт");
  }
}

std::int64_t AttemptRepository::StartSession(std::int64_t user_id, std::string_view mode) {
  EnsureUser(user_id);
  auto connection = impl_->Acquire();

  const auto result = ExecuteParams(connection.Get(),
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

std::optional<ActiveTrainingSessionRecord> AttemptRepository::LoadActiveTrainingSession(std::int64_t user_id) {
  auto connection = impl_->Acquire();
  const auto result = ExecuteParams(connection.Get(),
                                    R"sql(
SELECT active.session_id,
       active.state_version,
       active.content_version,
       active.state_json::TEXT
FROM active_training_sessions AS active
JOIN sessions AS session
  ON session.id = active.session_id
 AND session.user_id = active.user_id
WHERE active.user_id = $1
  AND active.expires_at > now()
  AND session.finished_at IS NULL;
)sql",
                                    {std::to_string(user_id)}, true);
  if (PQnfields(result.Get()) != 4 || PQntuples(result.Get()) > 1) {
    throw std::runtime_error("unexpected PostgreSQL result when loading active session");
  }
  if (PQntuples(result.Get()) == 0) {
    return std::nullopt;
  }
  return ActiveTrainingSessionRecord{
      .session_id = ParseInt64(PQgetvalue(result.Get(), 0, 0)),
      .state_version = ParseInt64(PQgetvalue(result.Get(), 0, 1)),
      .content_version = PQgetvalue(result.Get(), 0, 2),
      .state_json = PQgetvalue(result.Get(), 0, 3),
  };
}

std::int64_t AttemptRepository::StartActiveTrainingSession(std::int64_t user_id, std::string_view mode,
                                                           std::string_view content_version,
                                                           std::string_view state_json, std::string_view request_id) {
  if (user_id == 0 || mode.empty()) {
    throw std::invalid_argument("user id and active session mode must not be empty");
  }
  ValidateSessionSnapshot(content_version, state_json);
  const auto user_id_text = std::to_string(user_id);
  auto connection = impl_->Acquire();
  Transaction transaction(connection.Get());
  static_cast<void>(transaction.ExecuteParams(
      "INSERT INTO users(user_id) VALUES ($1) ON CONFLICT (user_id) DO NOTHING;", {user_id_text}, false));
  const auto locked =
      transaction.ExecuteParams("SELECT user_id FROM users WHERE user_id = $1 FOR UPDATE;", {user_id_text}, true);
  if (PQntuples(locked.Get()) != 1 || PQnfields(locked.Get()) != 1) {
    throw std::runtime_error("unexpected PostgreSQL result when locking active session owner");
  }

  if (!request_id.empty()) {
    const auto previous =
        transaction.ExecuteParams("SELECT id FROM sessions WHERE user_id = $1 AND client_request_id = $2;",
                                  {user_id_text, std::string(request_id)}, true);
    if (PQntuples(previous.Get()) != 0) {
      const auto previous_id = ParseInt64(PQgetvalue(previous.Get(), 0, 0));
      transaction.Commit();
      return previous_id;
    }
  }

  static_cast<void>(transaction.ExecuteParams(
      R"sql(
WITH expired AS (
    UPDATE sessions s SET finished_at = now(), finish_reason = 'expired'
    FROM active_training_sessions a
    WHERE a.session_id = s.id AND a.user_id = $1 AND a.expires_at <= now() AND s.finished_at IS NULL
    RETURNING s.id
)
DELETE FROM active_training_sessions WHERE session_id IN (SELECT id FROM expired);
)sql",
      {user_id_text}, false));
  // The database mirror retains the previous snapshot, including writes made by
  // the preceding binary during a rollback. Only the current pointer is removed.
  static_cast<void>(
      transaction.ExecuteParams("DELETE FROM active_training_sessions WHERE user_id = $1;", {user_id_text}, false));
  const auto session = transaction.ExecuteParams(
      "INSERT INTO sessions(user_id, mode, client_request_id) VALUES ($1, $2, NULLIF($3, '')) RETURNING id;",
      {user_id_text, std::string(mode), std::string(request_id)}, true);
  if (PQntuples(session.Get()) != 1 || PQnfields(session.Get()) != 1) {
    throw std::runtime_error("unexpected PostgreSQL result when starting active session");
  }
  const auto session_id = ParseInt64(PQgetvalue(session.Get(), 0, 0));
  static_cast<void>(transaction.ExecuteParams(
      R"sql(
INSERT INTO active_training_sessions(
    user_id, session_id, state_json, state_version, content_version, updated_at, expires_at
)
VALUES ($1, $2, $3::JSONB, 1, $4, now(), 'infinity');
)sql",
      {user_id_text, std::to_string(session_id), std::string(state_json), std::string(content_version)}, false));
  transaction.Commit();
  return session_id;
}

std::vector<ActiveTrainingSessionRecord> AttemptRepository::LoadSavedTrainingSessions(std::int64_t user_id) {
  auto connection = impl_->Acquire();
  const auto result = ExecuteParams(connection.Get(), R"sql(
SELECT a.session_id, a.state_version, a.content_version, a.state_json::TEXT
FROM saved_training_sessions a JOIN sessions s ON s.id = a.session_id AND s.user_id = a.user_id
WHERE a.user_id = $1 AND s.finished_at IS NULL
  AND NOT EXISTS (SELECT 1 FROM active_training_sessions current WHERE current.session_id = a.session_id)
ORDER BY a.updated_at DESC, a.session_id DESC;
)sql",
                                    {std::to_string(user_id)}, true);
  std::vector<ActiveTrainingSessionRecord> saved;
  for (int row = 0; row < PQntuples(result.Get()); ++row) {
    saved.push_back({.session_id = ParseInt64(PQgetvalue(result.Get(), row, 0)),
                     .state_version = ParseInt64(PQgetvalue(result.Get(), row, 1)),
                     .content_version = PQgetvalue(result.Get(), row, 2),
                     .state_json = PQgetvalue(result.Get(), row, 3)});
  }
  return saved;
}

std::int64_t AttemptRepository::ResumeTrainingSession(std::int64_t user_id, std::int64_t session_id,
                                                      std::int64_t expected_version, std::string_view content_version,
                                                      std::string_view state_json) {
  ValidateSessionSnapshot(content_version, state_json);
  auto connection = impl_->Acquire();
  Transaction transaction(connection.Get());
  const auto owner = std::to_string(user_id);
  static_cast<void>(
      transaction.ExecuteParams("SELECT user_id FROM users WHERE user_id = $1 FOR UPDATE;", {owner}, true));
  static_cast<void>(
      transaction.ExecuteParams("DELETE FROM active_training_sessions WHERE user_id = $1;", {owner}, false));
  const auto result = transaction.ExecuteParams(R"sql(
INSERT INTO active_training_sessions(user_id, session_id, state_json, state_version, content_version, updated_at, expires_at)
SELECT a.user_id, a.session_id, $5::JSONB, a.state_version + 1, $4, now(), 'infinity'
FROM saved_training_sessions a
WHERE a.user_id = $1 AND a.session_id = $2 AND a.state_version = $3
  AND a.content_version = $4
  AND EXISTS (SELECT 1 FROM sessions s WHERE s.id = a.session_id AND s.finished_at IS NULL)
RETURNING state_version;
)sql",
                                                {owner, std::to_string(session_id), std::to_string(expected_version),
                                                 std::string(content_version), std::string(state_json)},
                                                true);
  if (PQntuples(result.Get()) != 1)
    throw std::logic_error("Сохранённая тренировка изменилась или недоступна.");
  static_cast<void>(transaction.ExecuteParams(
      "UPDATE sessions SET finish_reason = NULL WHERE user_id = $1 AND id = $2 AND finished_at IS NULL;",
      {owner, std::to_string(session_id)}, false));
  const auto version = ParseInt64(PQgetvalue(result.Get(), 0, 0));
  transaction.Commit();
  return version;
}

void AttemptRepository::PreserveUnavailableTraining(std::int64_t user_id, std::int64_t session_id,
                                                    std::int64_t expected_version, std::string_view reason) {
  if (reason != "snapshot_invalid" && reason != "snapshot_incompatible") {
    throw std::invalid_argument("invalid unavailable training reason");
  }
  auto connection = impl_->Acquire();
  Transaction transaction(connection.Get());
  static_cast<void>(transaction.ExecuteParams(
      R"sql(
WITH parked AS (
    DELETE FROM active_training_sessions
    WHERE user_id = $1 AND session_id = $2 AND state_version = $3
    RETURNING session_id
)
UPDATE sessions SET finish_reason = $4 WHERE id IN (SELECT session_id FROM parked) AND finished_at IS NULL;
)sql",
      {std::to_string(user_id), std::to_string(session_id), std::to_string(expected_version), std::string(reason)},
      false));
  transaction.Commit();
}

std::vector<ReferenceViewRecord> AttemptRepository::LoadReferenceViews(std::int64_t user_id, std::int64_t session_id,
                                                                       std::string_view question_id,
                                                                       std::string_view question_content_revision) {
  auto connection = impl_->Acquire();
  const auto result = ExecuteParams(connection.Get(), R"sql(
SELECT v.id, v.reference_id, v.reference_content_revision, r.content_snapshot::TEXT, v.attempt_number,
       to_char(v.opened_at AT TIME ZONE 'UTC', 'YYYY-MM-DD"T"HH24:MI:SS.MS"Z"')
FROM reference_views v JOIN reference_content_revisions r
    ON r.reference_id = v.reference_id AND r.content_revision = v.reference_content_revision
WHERE v.user_id = $1 AND v.session_id = $2 AND v.question_id = $3 AND v.question_content_revision = $4
ORDER BY v.id;
)sql",
                                    {std::to_string(user_id), std::to_string(session_id), std::string(question_id),
                                     std::string(question_content_revision)},
                                    true);
  std::vector<ReferenceViewRecord> views;
  for (int row = 0; row < PQntuples(result.Get()); ++row) {
    views.push_back({.id = ParseInt64(PQgetvalue(result.Get(), row, 0)),
                     .reference_id = PQgetvalue(result.Get(), row, 1),
                     .reference_content_revision = PQgetvalue(result.Get(), row, 2),
                     .reference_snapshot_json = PQgetvalue(result.Get(), row, 3),
                     .attempt_number = static_cast<std::size_t>(ParseInt64(PQgetvalue(result.Get(), row, 4))),
                     .opened_at = PQgetvalue(result.Get(), row, 5)});
  }
  return views;
}

std::int64_t AttemptRepository::UpdateActiveTrainingSession(std::int64_t user_id, std::int64_t session_id,
                                                            std::int64_t expected_version,
                                                            std::string_view content_version,
                                                            std::string_view state_json,
                                                            std::optional<ReviewFailureContext> review_failure,
                                                            std::optional<ReferenceViewContext> reference_view) {
  if (review_failure && (review_failure->question_id.empty() || review_failure->skill_id.empty())) {
    throw std::invalid_argument("review failure context is incomplete");
  }
  if (reference_view) {
    const auto& view = *reference_view;
    ValidateFeedbackText(view.question_id, 256);
    ValidateFeedbackText(view.reference_id, 64);
    ValidateFeedbackText(view.reference_snapshot_json, 65536);
    const auto content = boost::json::parse(view.reference_snapshot_json);
    const auto snapshot = boost::json::parse(state_json).as_object();
    const auto& marked = snapshot.at("reference_viewed_question_ids").as_array();
    if (view.question_content_revision.size() != 64 || view.reference_content_revision.size() != 64 ||
        view.attempt_number < 1 || view.attempt_number > 1000 || !content.is_object() ||
        content.as_object().at("id").as_string() != view.reference_id ||
        QuestionContentRevision(CanonicalQuestionJson(content)) != view.reference_content_revision ||
        snapshot.at("current_content_revision").as_string() != view.question_content_revision ||
        snapshot.at("quiz").as_object().at("answered").as_bool() ||
        snapshot.at("current_attempt").as_int64() != static_cast<std::int64_t>(view.attempt_number) ||
        std::ranges::none_of(marked, [&view](const auto& id) { return id.as_string() == view.question_id; })) {
      throw std::invalid_argument("invalid reference provenance or missing assistance marker");
    }
  }
  auto connection = impl_->Acquire();
  Transaction transaction(connection.Get());
  if (review_failure || reference_view) {
    const auto user_lock = transaction.ExecuteParams("SELECT user_id FROM users WHERE user_id = $1 FOR UPDATE;",
                                                     {std::to_string(user_id)}, true);
    if (PQntuples(user_lock.Get()) != 1) {
      throw std::logic_error("session update references an unknown user");
    }
  }
  const auto next_version =
      UpdateActiveSessionSnapshot(transaction, user_id, session_id, expected_version, content_version, state_json);
  if (review_failure) {
    static_cast<void>(RecordReviewFailure(transaction, user_id, *review_failure));
  }
  if (reference_view) {
    const auto& view = *reference_view;
    const auto archived =
        transaction.ExecuteParams(R"sql(
INSERT INTO reference_content_revisions(reference_id, content_revision, content_snapshot)
VALUES ($1, $2, $3::JSONB)
ON CONFLICT (reference_id, content_revision) DO UPDATE SET content_revision = EXCLUDED.content_revision
WHERE reference_content_revisions.content_snapshot = EXCLUDED.content_snapshot
RETURNING reference_id;
)sql",
                                  {std::string(view.reference_id), std::string(view.reference_content_revision),
                                   std::string(view.reference_snapshot_json)},
                                  true);
    if (PQntuples(archived.Get()) != 1) {
      throw std::logic_error("a reference revision already refers to different content");
    }
    static_cast<void>(
        transaction.ExecuteParams(R"sql(
INSERT INTO reference_views(user_id, session_id, question_id, question_content_revision,
                            reference_id, reference_content_revision, attempt_number)
VALUES ($1, $2, $3, $4, $5, $6, $7)
ON CONFLICT (session_id, question_id, question_content_revision, reference_id, reference_content_revision) DO NOTHING;
)sql",
                                  {std::to_string(user_id), std::to_string(session_id), std::string(view.question_id),
                                   std::string(view.question_content_revision), std::string(view.reference_id),
                                   std::string(view.reference_content_revision), std::to_string(view.attempt_number)},
                                  false));
  }
  transaction.Commit();
  return next_version;
}

RecordSubmissionOutcome AttemptRepository::RecordSubmission(const SubmissionRecord& submission) {
  return RecordSubmissionImpl(submission, nullptr);
}

RecordSubmissionOutcome AttemptRepository::RecordSubmission(const SubmissionRecord& submission,
                                                            const ActiveSessionUpdate& active_session_update) {
  return RecordSubmissionImpl(submission, &active_session_update);
}

RecordSubmissionOutcome AttemptRepository::RecordSubmissionImpl(const SubmissionRecord& submission,
                                                                const ActiveSessionUpdate* active_session_update) {
  if (submission.user_id == 0 || submission.session_id == 0) {
    throw std::invalid_argument("user and session ids must not be zero");
  }

  if (submission.question_id.empty() || submission.skill_id.empty() || submission.question_family_id.empty() ||
      submission.answer.empty()) {
    throw std::invalid_argument("question id, skill id, question family id and answer must not be empty");
  }

  if (submission.attempt_number < 1 || submission.attempt_number > 1'000) {
    throw std::invalid_argument("attempt number must be between 1 and 1000");
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
  if (active_session_update != nullptr &&
      (active_session_update->expected_version <= 0 || active_session_update->content_version.empty() ||
       !active_session_update->build_state_json)) {
    throw std::invalid_argument("active session update is incomplete");
  }

  const auto user_id = std::to_string(submission.user_id);
  const auto session_id = std::to_string(submission.session_id);
  const auto attempt_number = std::to_string(submission.attempt_number);
  const auto correct = submission.correct ? "true" : "false";
  const auto completed_increment = submission.completed ? "1" : "0";
  const auto correct_increment = submission.completed && submission.correct ? "1" : "0";
  const auto failed_increment = submission.completed && !submission.correct ? "1" : "0";
  const auto first_try_increment =
      submission.completed && submission.correct && submission.attempt_number == 1 && !submission.assisted ? "1" : "0";
  const auto evidence_type = std::string(QuestionEvidenceTypeName(submission.evidence_type));
  const auto transfer = submission.transfer ? "true" : "false";
  const auto assisted = submission.assisted ? "true" : "false";

  auto connection = impl_->Acquire();
  Transaction transaction(connection.Get());

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
        FROM user_skill_state AS state
        JOIN sessions AS current_session
          ON current_session.id = $5
         AND current_session.user_id = state.user_id
        WHERE state.user_id = $1
          AND state.skill_id = $3
          AND state.status = 'demonstrated'
          AND state.demonstrated_at <= now() - INTERVAL '1 day'
          AND current_session.started_at > state.demonstrated_at
    ),
    EXISTS(
        SELECT 1
        FROM user_skill_state
        WHERE user_id = $1
          AND skill_id = $3
          AND status = 'needs_recovery'
          AND needs_recovery_since IS NOT NULL
          AND needs_recovery_since < now()
    ),
    EXISTS(
        SELECT 1
        FROM skill_evidence
        WHERE user_id = $1
          AND skill_id = $3
          AND question_family_id = $4
    ),
    EXISTS(
        SELECT 1
        FROM attempts
        WHERE user_id = $1
          AND question_id = $2
    ),
    COALESCE((
        SELECT review_stage FROM question_progress WHERE user_id = $1 AND question_id = $2
    ), 0);
)sql",
        {
            user_id,
            std::string(submission.question_id),
            std::string(submission.skill_id),
            std::string(submission.question_family_id),
            session_id,
        },
        true);

    if (PQntuples(context.Get()) != 1 || PQnfields(context.Get()) != 8) {
      throw std::runtime_error("unexpected PostgreSQL result when reading skill evidence context");
    }

    const bool review_was_due = ParseBool(PQgetvalue(context.Get(), 0, 0));
    const std::string review_cycle = PQgetvalue(context.Get(), 0, 1);
    const auto previous_mastery = ParseSkillMasteryStatus(PQgetvalue(context.Get(), 0, 2));
    const bool retention_window_open = ParseBool(PQgetvalue(context.Get(), 0, 3));
    const bool recovery_window_open = ParseBool(PQgetvalue(context.Get(), 0, 4));
    const bool family_seen_before = ParseBool(PQgetvalue(context.Get(), 0, 5));
    const bool question_seen_before = ParseBool(PQgetvalue(context.Get(), 0, 6));
    const auto previous_review_stage = ParseInt64(PQgetvalue(context.Get(), 0, 7));
    if (previous_review_stage < 0 || previous_review_stage > 5) {
      throw std::runtime_error("stored review stage must be between 0 and 5");
    }

    const auto attempt = transaction.ExecuteParams(
        R"sql(
INSERT INTO attempts(
    user_id,
    session_id,
    question_id,
    answer,
    correct,
    attempt_number,
    assisted,
    content_revision
)
SELECT $1, $2, $3, $4, $5, $6, $7, $8
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
            assisted,
            std::string(submission.content_revision),
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
          .rewards_unlocked = {},
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
    std::vector<std::string> rewards_unlocked;

    if (!submission.correct && !submission.completed) {
      mastery_change =
          RecordReviewFailure(transaction, submission.user_id, {submission.question_id, submission.skill_id});
    }

    if (submission.completed) {
      const bool clean_success = submission.correct && submission.attempt_number == 1 && !submission.assisted;
      const bool clean_application = clean_success && submission.evidence_type == QuestionEvidenceType::kApplication;

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
    first_recorded_at,
    last_recorded_at
)
VALUES ($1, $2, $3, $4, $5, $6, $7, $8, now(), now())
ON CONFLICT (user_id, skill_id, question_family_id)
DO UPDATE SET
    question_id = EXCLUDED.question_id,
    successful = evidence.successful OR EXCLUDED.successful,
    clean_success = evidence.clean_success OR EXCLUDED.clean_success,
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
          },
          false));

      const auto evidence = transaction.ExecuteParams(
          R"sql(
SELECT
    COUNT(*) FILTER (WHERE successful),
    COUNT(*) FILTER (WHERE clean_success),
    COUNT(*) FILTER (WHERE clean_success AND evidence_type = 'application'),
    COUNT(*) FILTER (WHERE clean_success AND transfer)
FROM skill_evidence
WHERE user_id = $1
  AND skill_id = $2;
)sql",
          {
              user_id,
              std::string(submission.skill_id),
          },
          true);

      if (PQntuples(evidence.Get()) != 1 || PQnfields(evidence.Get()) != 4) {
        throw std::runtime_error("unexpected PostgreSQL result when aggregating skill evidence");
      }

      const SkillEvidenceSummary evidence_summary{
          .successful_family_count = ParseInt64(PQgetvalue(evidence.Get(), 0, 0)),
          .clean_family_count = ParseInt64(PQgetvalue(evidence.Get(), 0, 1)),
          .application_family_count = ParseInt64(PQgetvalue(evidence.Get(), 0, 2)),
          .transfer_family_count = ParseInt64(PQgetvalue(evidence.Get(), 0, 3)),
      };
      const auto mastery = ApplySkillMastery(
          previous_mastery, evidence_summary,
          SkillMasteryEvent{
              .due_failure = review_was_due && (!submission.correct || submission.attempt_number > 1),
              .clean_application = clean_application,
              .recovery_evidence_after_start = recovery_window_open,
              .delayed_retention_evidence = retention_window_open && (!question_seen_before || review_was_due),
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
    demonstrated_at = CASE
        WHEN state.status = 'needs_recovery' AND EXCLUDED.status = 'demonstrated'
            THEN now()
        ELSE COALESCE(state.demonstrated_at, EXCLUDED.demonstrated_at)
    END,
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

      if (submission.correct && !submission.assisted) {
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

      const auto review_schedule = PlanQuestionReview(static_cast<int>(previous_review_stage), submission.correct,
                                                      submission.attempt_number, submission.assisted);
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
    $6,
    $7,
    now() + $8::BIGINT * INTERVAL '1 minute',
    now()
)
ON CONFLICT (user_id, question_id)
DO UPDATE SET
    completed_count = progress.completed_count + 1,
    correct_count = progress.correct_count + EXCLUDED.correct_count,
    failed_count = progress.failed_count + EXCLUDED.failed_count,
    first_try_correct_count =
        progress.first_try_correct_count + EXCLUDED.first_try_correct_count,
    review_stage = EXCLUDED.review_stage,
    consecutive_correct = CASE
        WHEN $7::INTEGER = 1 THEN progress.consecutive_correct + 1
        ELSE 0
    END,
    next_review_at = EXCLUDED.next_review_at,
    last_answered_at = now();
)sql",
          {
              user_id,
              std::string(submission.question_id),
              correct_increment,
              failed_increment,
              first_try_increment,
              std::to_string(review_schedule.stage),
              first_try_increment,
              std::to_string(review_schedule.delay.count()),
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

    if (xp_gained > 0) {
      rewards_unlocked = GrantQualifiedRewards(transaction, user_id, true);
    }

    RecordSubmissionOutcome outcome{
        .result = RecordSubmissionResult::kInserted,
        .skill_mastery = std::move(mastery_change),
        .xp_gained = xp_gained,
        .rewards_unlocked = std::move(rewards_unlocked),
    };
    if (active_session_update != nullptr) {
      const auto state_json = active_session_update->build_state_json(outcome);
      static_cast<void>(UpdateActiveSessionSnapshot(transaction, submission.user_id, submission.session_id,
                                                    active_session_update->expected_version,
                                                    active_session_update->content_version, state_json));
    }
    transaction.Commit();
    return outcome;
  }
}

std::optional<std::string> AttemptRepository::LoadTrainingCompletion(std::int64_t user_id, std::int64_t session_id,
                                                                     std::optional<std::int64_t> total_attempts) {
  auto connection = impl_->Acquire();
  const auto result = ExecuteParams(connection.Get(), R"sql(
SELECT r.receipt_json::TEXT FROM training_completion_receipts r
JOIN sessions s ON s.id = r.session_id AND s.user_id = r.user_id
WHERE r.user_id = $1 AND r.session_id = $2 AND s.finish_reason = 'completed'
  AND (NULLIF($3, '')::BIGINT IS NULL OR r.total_attempts = NULLIF($3, '')::BIGINT);
)sql",
                                    {std::to_string(user_id), std::to_string(session_id),
                                     total_attempts ? std::to_string(*total_attempts) : std::string{}},
                                    true);
  if (PQntuples(result.Get()) == 0)
    return std::nullopt;
  return std::string(PQgetvalue(result.Get(), 0, 0));
}

std::string AttemptRepository::CompleteActiveTrainingSession(
    std::int64_t user_id, std::int64_t session_id, std::int64_t expected_version, std::int64_t total_attempts,
    const std::function<std::string(const UserProgressSummary&)>& build_receipt) {
  if (user_id == 0 || session_id <= 0 || expected_version <= 0 || total_attempts <= 0 || !build_receipt)
    throw std::invalid_argument("completion identity, cursor and builder are required");
  auto connection = impl_->Acquire();
  Transaction transaction(connection.Get());
  const std::vector<std::string> identity{std::to_string(user_id), std::to_string(session_id),
                                          std::to_string(expected_version), std::to_string(total_attempts)};
  // Same lock order as answers and starts, including across service instances.
  const auto owner =
      transaction.ExecuteParams("SELECT user_id FROM users WHERE user_id = $1 FOR UPDATE;", {identity[0]}, true);
  if (PQntuples(owner.Get()) != 1)
    throw std::logic_error("training is no longer available");
  const auto previous = transaction.ExecuteParams(
      "SELECT receipt_json::TEXT, total_attempts FROM training_completion_receipts "
      "WHERE user_id = $1 AND session_id = $2;",
      {identity[0], identity[1]}, true);
  if (PQntuples(previous.Get()) != 0) {
    if (ParseInt64(PQgetvalue(previous.Get(), 0, 1)) != total_attempts)
      throw std::logic_error("completion cursor does not match");
    std::string receipt(PQgetvalue(previous.Get(), 0, 0));
    transaction.Commit();
    return receipt;
  }
  const auto finished = transaction.ExecuteParams(R"sql(
UPDATE sessions s SET finished_at = now(), finish_reason = 'completed'
FROM active_training_sessions a
WHERE s.id = $2 AND s.user_id = $1 AND s.finished_at IS NULL
  AND a.user_id = $1 AND a.session_id = s.id AND a.state_version = $3
RETURNING s.id;
)sql",
                                                  {identity[0], identity[1], identity[2]}, true);
  if (PQntuples(finished.Get()) != 1)
    throw std::logic_error("active training changed or is no longer available");
  const auto receipt = build_receipt(ReadProgressSummary(connection.Get(), user_id, ReconnectPolicy::kForbid));
  ValidateSessionSnapshot("completion-v1", receipt);
  static_cast<void>(transaction.ExecuteParams(R"sql(
INSERT INTO training_completion_receipts(user_id, session_id, total_attempts, receipt_json)
VALUES ($1, $2, $3, $4);
)sql",
                                              {identity[0], identity[1], identity[3], receipt}, false));
  static_cast<void>(
      transaction.ExecuteParams("DELETE FROM active_training_sessions WHERE user_id = $1 AND session_id = $2;",
                                {identity[0], identity[1]}, false));
  transaction.Commit();
  return receipt;
}

void AttemptRepository::FinishSession(std::int64_t session_id, std::string_view finish_reason) {
  if (finish_reason.empty()) {
    throw std::invalid_argument("finish reason must not be empty");
  }

  auto connection = impl_->Acquire();
  ExecuteParams(connection.Get(),
                R"sql(
WITH finished AS (
    UPDATE sessions
    SET finished_at = now(),
        finish_reason = $2
    WHERE id = $1
      AND finished_at IS NULL
    RETURNING id
)
DELETE FROM active_training_sessions
WHERE session_id IN (SELECT id FROM finished);
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

  auto connection = impl_->Acquire();
  ExecuteParams(connection.Get(),
                R"sql(
WITH finished AS (
    UPDATE sessions
    SET finished_at = now(),
        finish_reason = $2
    WHERE user_id = $1
      AND finished_at IS NULL
    RETURNING id
)
DELETE FROM active_training_sessions
WHERE session_id IN (SELECT id FROM finished);
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

  auto connection = impl_->Acquire();
  ExecuteParams(connection.Get(),
                R"sql(
WITH finished AS (
    UPDATE sessions
    SET finished_at = now(),
        finish_reason = $3
    WHERE user_id = $1
      AND left(mode, char_length($2)) = $2
      AND finished_at IS NULL
    RETURNING id
)
DELETE FROM active_training_sessions
WHERE session_id IN (SELECT id FROM finished);
)sql",
                {
                    std::to_string(user_id),
                    std::string(mode_prefix),
                    std::string(finish_reason),
                },
                false);
}
