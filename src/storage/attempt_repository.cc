#include "attempt_repository.h"

#include <libpq-fe.h>

#include <charconv>
#include <cstddef>
#include <cstdint>
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

void Execute(PGconn* connection, const char* sql) {
  Result result(PQexec(connection, sql));

  CheckCommand(connection, result, "PostgreSQL command failed");
}

Result ExecuteParams(PGconn* connection, const char* sql, const std::vector<std::string>& params, bool expect_rows) {
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

    Execute(connection,
            R"sql(
CREATE TABLE IF NOT EXISTS users (
    user_id BIGINT PRIMARY KEY,
    created_at TIMESTAMPTZ NOT NULL DEFAULT now(),
    target_date DATE,
    adaptive_level INTEGER NOT NULL DEFAULT 3
);

ALTER TABLE users
    ADD COLUMN IF NOT EXISTS adaptive_level INTEGER NOT NULL DEFAULT 3;

CREATE TABLE IF NOT EXISTS sessions (
    id BIGSERIAL PRIMARY KEY,
    user_id BIGINT NOT NULL
        REFERENCES users(user_id),
    started_at TIMESTAMPTZ NOT NULL DEFAULT now(),
    finished_at TIMESTAMPTZ,
    mode TEXT NOT NULL,
    finish_reason TEXT
);

ALTER TABLE sessions
    ADD COLUMN IF NOT EXISTS finish_reason TEXT;

CREATE INDEX IF NOT EXISTS
    idx_sessions_user_started
    ON sessions(user_id, started_at);

CREATE TABLE IF NOT EXISTS attempts (
    id BIGSERIAL PRIMARY KEY,
    user_id BIGINT NOT NULL
        REFERENCES users(user_id),
    session_id BIGINT NOT NULL
        REFERENCES sessions(id),
    question_id TEXT NOT NULL,
    answer TEXT NOT NULL,
    correct BOOLEAN NOT NULL,
    attempt_number INTEGER NOT NULL
        CHECK (attempt_number BETWEEN 1 AND 3),
    answered_at TIMESTAMPTZ NOT NULL DEFAULT now()
);

CREATE INDEX IF NOT EXISTS
    idx_attempts_user_question_time
    ON attempts(
        user_id,
        question_id,
        answered_at
    );

CREATE INDEX IF NOT EXISTS
    idx_attempts_user_time
    ON attempts(
        user_id,
        answered_at
    );

CREATE TABLE IF NOT EXISTS question_progress (
    user_id BIGINT NOT NULL
        REFERENCES users(user_id),
    question_id TEXT NOT NULL,
    completed_count INTEGER NOT NULL DEFAULT 0
        CHECK (completed_count >= 0),
    correct_count INTEGER NOT NULL DEFAULT 0
        CHECK (correct_count >= 0),
    failed_count INTEGER NOT NULL DEFAULT 0
        CHECK (failed_count >= 0),
    first_try_correct_count INTEGER NOT NULL DEFAULT 0
        CHECK (first_try_correct_count >= 0),
    last_answered_at TIMESTAMPTZ NOT NULL DEFAULT now(),
    PRIMARY KEY (user_id, question_id)
);

CREATE INDEX IF NOT EXISTS
    idx_question_progress_user_last
    ON question_progress(
        user_id,
        last_answered_at
    );

CREATE TABLE IF NOT EXISTS topic_progress (
    user_id BIGINT NOT NULL
        REFERENCES users(user_id),
    topic TEXT NOT NULL,
    adaptive_level INTEGER NOT NULL DEFAULT 3
        CHECK (adaptive_level BETWEEN 1 AND 10),
    completed_count INTEGER NOT NULL DEFAULT 0
        CHECK (completed_count >= 0),
    correct_count INTEGER NOT NULL DEFAULT 0
        CHECK (correct_count >= 0),
    failed_count INTEGER NOT NULL DEFAULT 0
        CHECK (failed_count >= 0),
    first_try_correct_count INTEGER NOT NULL DEFAULT 0
        CHECK (first_try_correct_count >= 0),
    last_answered_at TIMESTAMPTZ NOT NULL DEFAULT now(),
    PRIMARY KEY (user_id, topic)
);

CREATE INDEX IF NOT EXISTS
    idx_topic_progress_user_last
    ON topic_progress(
        user_id,
        last_answered_at
    );
)sql");
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

int AttemptRepository::GetAdaptiveLevel(std::int64_t user_id) {
  EnsureUser(user_id);

  const auto result = ExecuteParams(
      impl_->connection,
      R"sql(
SELECT adaptive_level
FROM users
WHERE user_id = $1;
)sql",
      {
          std::to_string(user_id),
      },
      true);

  if (PQntuples(result.Get()) != 1 ||
      PQnfields(result.Get()) != 1) {
    throw std::runtime_error(
        "unexpected PostgreSQL result when reading adaptive level");
  }

  const auto level = ParseInt64(
      PQgetvalue(result.Get(), 0, 0));

  if (level < 1 || level > 10) {
    throw std::runtime_error(
        "stored adaptive level must be between 1 and 10");
  }

  return static_cast<int>(level);
}

void AttemptRepository::SetAdaptiveLevel(
    std::int64_t user_id,
    int level) {
  if (level < 1 || level > 10) {
    throw std::invalid_argument(
        "adaptive level must be between 1 and 10");
  }

  EnsureUser(user_id);

  ExecuteParams(
      impl_->connection,
      R"sql(
UPDATE users
SET adaptive_level = $2
WHERE user_id = $1;
)sql",
      {
          std::to_string(user_id),
          std::to_string(level),
      },
      false);
}

std::unordered_map<std::string, int>
AttemptRepository::LoadTopicLevels(
    std::int64_t user_id) {
  EnsureUser(user_id);

  const auto result = ExecuteParams(
      impl_->connection,
      R"sql(
SELECT topic, adaptive_level
FROM topic_progress
WHERE user_id = $1;
)sql",
      {
          std::to_string(user_id),
      },
      true);

  std::unordered_map<std::string, int> levels;

  const auto row_count = PQntuples(result.Get());

  levels.reserve(static_cast<std::size_t>(row_count));

  for (int row = 0; row < row_count; ++row) {
    const auto* topic_value =
        PQgetvalue(result.Get(), row, 0);

    if (topic_value == nullptr || *topic_value == '\0') {
      throw std::runtime_error(
          "stored topic must not be empty");
    }

    const auto level =
        ParseInt64(
            PQgetvalue(result.Get(), row, 1));

    if (level < 1 || level > 10) {
      throw std::runtime_error(
          "stored topic level must be between 1 and 10");
    }

    levels.emplace(
        topic_value,
        static_cast<int>(level));
  }

  return levels;
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

void AttemptRepository::RecordAttempt(std::int64_t user_id, std::int64_t session_id, std::string_view question_id,
                                      std::string_view answer, bool correct, std::size_t attempt_number) {
  if (attempt_number < 1 || attempt_number > 3) {
    throw std::invalid_argument("attempt number must be between 1 and 3");
  }

  ExecuteParams(impl_->connection,
                R"sql(
INSERT INTO attempts(
    user_id,
    session_id,
    question_id,
    answer,
    correct,
    attempt_number
)
VALUES ($1, $2, $3, $4, $5, $6);
)sql",
                {
                    std::to_string(user_id),
                    std::to_string(session_id),
                    std::string(question_id),
                    std::string(answer),
                    correct ? "true" : "false",
                    std::to_string(attempt_number),
                },
                false);
}

void AttemptRepository::RecordQuestionCompletion(
    std::int64_t user_id,
    std::string_view question_id,
    std::string_view topic,
    bool correct,
    std::size_t attempts,
    int topic_level) {
  if (question_id.empty()) {
    throw std::invalid_argument(
        "question id must not be empty");
  }

  if (topic.empty()) {
    throw std::invalid_argument(
        "topic must not be empty");
  }

  if (attempts < 1 || attempts > 3) {
    throw std::invalid_argument(
        "attempt number must be between 1 and 3");
  }

  if (topic_level < 1 || topic_level > 10) {
    throw std::invalid_argument(
        "topic level must be between 1 and 10");
  }

  EnsureUser(user_id);

  const auto correct_increment =
      correct ? "1" : "0";

  const auto failed_increment =
      correct ? "0" : "1";

  const auto first_try_increment =
      correct && attempts == 1 ? "1" : "0";

  Execute(impl_->connection, "BEGIN");

  try {
    ExecuteParams(
        impl_->connection,
        R"sql(
INSERT INTO question_progress AS progress(
    user_id,
    question_id,
    completed_count,
    correct_count,
    failed_count,
    first_try_correct_count,
    last_answered_at
)
VALUES ($1, $2, 1, $3, $4, $5, now())
ON CONFLICT (user_id, question_id)
DO UPDATE SET
    completed_count =
        progress.completed_count + 1,
    correct_count =
        progress.correct_count + EXCLUDED.correct_count,
    failed_count =
        progress.failed_count + EXCLUDED.failed_count,
    first_try_correct_count =
        progress.first_try_correct_count +
        EXCLUDED.first_try_correct_count,
    last_answered_at = now();
)sql",
        {
            std::to_string(user_id),
            std::string(question_id),
            correct_increment,
            failed_increment,
            first_try_increment,
        },
        false);

    ExecuteParams(
        impl_->connection,
        R"sql(
INSERT INTO topic_progress AS progress(
    user_id,
    topic,
    adaptive_level,
    completed_count,
    correct_count,
    failed_count,
    first_try_correct_count,
    last_answered_at
)
VALUES ($1, $2, $3, 1, $4, $5, $6, now())
ON CONFLICT (user_id, topic)
DO UPDATE SET
    adaptive_level = EXCLUDED.adaptive_level,
    completed_count =
        progress.completed_count + 1,
    correct_count =
        progress.correct_count + EXCLUDED.correct_count,
    failed_count =
        progress.failed_count + EXCLUDED.failed_count,
    first_try_correct_count =
        progress.first_try_correct_count +
        EXCLUDED.first_try_correct_count,
    last_answered_at = now();
)sql",
        {
            std::to_string(user_id),
            std::string(topic),
            std::to_string(topic_level),
            correct_increment,
            failed_increment,
            first_try_increment,
        },
        false);

    Execute(impl_->connection, "COMMIT");

  } catch (...) {
    try {
      Execute(impl_->connection, "ROLLBACK");
    } catch (...) {
    }

    throw;
  }
}

void AttemptRepository::FinishSession(
    std::int64_t session_id,
    std::string_view finish_reason) {
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

void AttemptRepository::FinishOpenSessions(
    std::int64_t user_id,
    std::string_view finish_reason) {
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
