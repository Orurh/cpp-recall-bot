#include "storage/attempt_repository.h"
#include "storage/postgres_connection.h"

#include <libpq-fe.h>

#include <chrono>
#include <future>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>

namespace {
using namespace std::chrono_literals;
using Result = std::unique_ptr<PGresult, decltype(&PQclear)>;
void Check(bool condition, const char* message) {
  if (!condition) {
    throw std::runtime_error(message);
  }
}
Result Sql(PGconn* connection, const std::string& sql) {
  Result result(PQexec(connection, sql.c_str()), PQclear);
  Check(result && (PQresultStatus(result.get()) == PGRES_COMMAND_OK || PQresultStatus(result.get()) == PGRES_TUPLES_OK),
        "database timeout SQL fixture failed");
  return result;
}
Result Run(PostgresConnection& connection, const char* sql) {
  return Result(connection.Execute(sql), PQclear);
}
std::string Scalar(PGconn* connection, const std::string& sql) {
  auto result = Sql(connection, sql);
  Check(PQntuples(result.get()) == 1, "database timeout scalar fixture failed");
  return PQgetvalue(result.get(), 0, 0);
}
template <typename Operation>
void Reject(Operation&& operation) {
  bool rejected = false;
  try {
    operation();
  } catch (const std::runtime_error&) {
    rejected = true;
  }
  Check(rejected, "a blocked database operation unexpectedly succeeded");
}
}  // namespace

void CheckDatabaseTimeouts(const char* url) {
  DatabaseTimeouts limits;
  limits.statement = 150ms;
  limits.lock = 50ms;
  limits.query = 600ms;
  limits.migration = 2s;
  PostgresConnection connection(url, limits);
  const auto settings = [&] {
    auto result = Run(connection, "SELECT current_setting('statement_timeout'), current_setting('lock_timeout');");
    Check(PQresultStatus(result.get()) == PGRES_TUPLES_OK && std::string(PQgetvalue(result.get(), 0, 0)) == "150ms" &&
              std::string(PQgetvalue(result.get(), 0, 1)) == "50ms",
          "database timeout settings were not restored");
  };
  settings();
  auto started = std::chrono::steady_clock::now();
  auto timed = Run(connection, "SELECT pg_sleep(1);");
  Check(PQresultStatus(timed.get()) == PGRES_FATAL_ERROR &&
            std::string(PQresultErrorField(timed.get(), PG_DIAG_SQLSTATE)) == "57014" &&
            std::chrono::steady_clock::now() - started < 1s,
        "server statement timeout was not applied");
  settings();  // An error response must be fully drained before reusing the connection.

  connection.SetMigrationMode(true);
  Check(PQresultStatus(Run(connection, "SELECT pg_sleep(0.25);").get()) == PGRES_TUPLES_OK,
        "the ordinary request timeout was incorrectly applied to migration mode");
  connection.SetMigrationMode(false);
  settings();

  // A client-side deadline closes the connection instead of recycling unread results.
  started = std::chrono::steady_clock::now();
  Reject([&] { Result result(connection.Execute("SELECT pg_sleep(1);", nullptr, true, 30ms), PQclear); });
  Check(std::chrono::steady_clock::now() - started < 500ms, "client database deadline did not bound the wait");
  settings();  // Reconnection must reapply the server settings.

  auto pool = std::make_shared<DatabaseConnectionPool>(url, 1, limits);
  AttemptRepository repository(pool);
  std::unique_ptr<PGconn, decltype(&PQfinish)> admin(PQconnectdb(url), PQfinish);
  Check(admin && PQstatus(admin.get()) == CONNECTION_OK, "database timeout admin fixture unavailable");
  const auto uid = -6'000'000'000 - std::chrono::steady_clock::now().time_since_epoch().count() % 1'000'000'000;
  const auto owner = std::to_string(uid);
  const auto session = repository.StartActiveTrainingSession(uid, "miniapp:timeout-test", "test", R"({"step":0})");
  const SubmissionRecord submission{
      .user_id = uid,
      .session_id = session,
      .question_id = "integration-timeout-answer",
      .topic = "integration",
      .skill_id = "integration.timeout",
      .question_family_id = "integration.timeout.family",
      .answer = "correct",
      .correct = true,
      .completed = true,
      .attempt_number = 1,
  };
  const ActiveSessionUpdate update{
      .expected_version = 1,
      .content_version = "test",
      .build_state_json = [](const auto&) { return std::string(R"({"step":1})"); },
  };

  static_cast<void>(Sql(admin.get(), "BEGIN; SELECT user_id FROM users WHERE user_id = " + owner + " FOR UPDATE;"));
  started = std::chrono::steady_clock::now();
  Reject([&] { static_cast<void>(repository.RecordSubmission(submission, update)); });
  Check(std::chrono::steady_clock::now() - started < 1s && pool->Available() == 1,
        "lock timeout did not release the pool lease");
  static_cast<void>(Sql(admin.get(), "ROLLBACK;"));

  // Timeout late in the transaction, after attempt / mastery / XP writes.
  static_cast<void>(
      Sql(admin.get(),
          "CREATE OR REPLACE FUNCTION cpp_recall_timeout_test() RETURNS trigger LANGUAGE plpgsql AS $$ BEGIN "
          "IF NEW.user_id = " +
              owner +
              " THEN PERFORM pg_sleep(1); END IF; RETURN NEW; END $$; "
              "CREATE TRIGGER cpp_recall_timeout_test BEFORE INSERT ON question_progress "
              "FOR EACH ROW EXECUTE FUNCTION cpp_recall_timeout_test();"));
  try {
    Reject([&] { static_cast<void>(repository.RecordSubmission(submission, update)); });
  } catch (...) {
    static_cast<void>(
        Sql(admin.get(),
            "DROP TRIGGER cpp_recall_timeout_test ON question_progress; DROP FUNCTION cpp_recall_timeout_test();"));
    throw;
  }
  static_cast<void>(
      Sql(admin.get(),
          "DROP TRIGGER cpp_recall_timeout_test ON question_progress; DROP FUNCTION cpp_recall_timeout_test();"));
  const auto unchanged = repository.LoadProgressSummary(uid);
  Check(unchanged.answers.attempts == 0 && unchanged.xp_total == 0 && unchanged.completed_count == 0 &&
            unchanged.skills.empty() && repository.LoadReviewStates(uid).empty() &&
            repository.LoadActiveTrainingSession(uid)->state_version == 1 && pool->Available() == 1,
        "database timeout partially committed attempt, progress, XP or snapshot");
  const auto retry = repository.RecordSubmission(submission, update);
  Check(retry.xp_gained == 12 && repository.RecordSubmission(submission, update).xp_gained == 0 &&
            repository.LoadActiveTrainingSession(uid)->state_version == 2,
        "retry after database timeout lost or duplicated a submission");

  DatabaseTimeouts stop_limits;
  auto stopping_pool = std::make_shared<DatabaseConnectionPool>(url, 1, stop_limits);
  AttemptRepository stopping(stopping_pool);
  static_cast<void>(Sql(admin.get(), "BEGIN; SELECT user_id FROM users WHERE user_id = " + owner + " FOR UPDATE;"));
  auto stopped_submission = submission;
  stopped_submission.question_id = "integration-stopped-answer";
  auto blocked = std::async(std::launch::async,
                            [&] { Reject([&] { static_cast<void>(stopping.RecordSubmission(stopped_submission)); }); });
  // RecordSubmission explicitly locks the user row before changing progress.
  const auto wait_deadline = std::chrono::steady_clock::now() + 1s;
  bool waiting = false;
  while (std::chrono::steady_clock::now() < wait_deadline) {
    waiting = Scalar(admin.get(),
                     "SELECT pg_stat_clear_snapshot(); SELECT EXISTS(SELECT 1 FROM pg_stat_activity "
                     "WHERE application_name = 'cpp-recall-bot' AND wait_event_type = 'Lock');") == "t";
    if (waiting) {
      break;
    }
    std::this_thread::sleep_for(5ms);
  }
  auto queued = std::async(std::launch::async, [&] { Reject([&] { stopping.EnsureUser(uid); }); });
  started = std::chrono::steady_clock::now();
  stopping_pool->RequestStop();
  static_cast<void>(Sql(admin.get(), "ROLLBACK;"));
  blocked.get();
  queued.get();
  Check(waiting && std::chrono::steady_clock::now() - started < 500ms && stopping_pool->Available() == 1 &&
            !stopping.CheckReady(),
        "database shutdown did not interrupt active and waiting operations");
}
