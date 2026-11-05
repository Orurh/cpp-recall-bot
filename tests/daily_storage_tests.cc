#include "storage/attempt_repository.h"

#include <libpq-fe.h>

#include <algorithm>
#include <chrono>
#include <future>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {
void Check(bool condition, const char* message) {
  if (!condition)
    throw std::runtime_error(message);
}

using QueryResult = std::unique_ptr<PGresult, decltype(&PQclear)>;

QueryResult Query(PGconn* db, const char* sql, const std::vector<std::string>& values = {}) {
  std::vector<const char*> parameters;
  for (const auto& value : values)
    parameters.push_back(value.c_str());
  QueryResult result(
      PQexecParams(db, sql, static_cast<int>(parameters.size()), nullptr, parameters.data(), nullptr, nullptr, 0),
      PQclear);
  Check(result && (PQresultStatus(result.get()) == PGRES_TUPLES_OK || PQresultStatus(result.get()) == PGRES_COMMAND_OK),
        "daily storage SQL fixture failed");
  return result;
}

bool BooleanQuery(PGconn* db, const char* sql, const std::vector<std::string>& parameters = {}) {
  const auto result = Query(db, sql, parameters);
  Check(PQntuples(result.get()) == 1 && PQnfields(result.get()) == 1, "daily boolean fixture returned invalid shape");
  return std::string_view(PQgetvalue(result.get(), 0, 0)) == "t";
}
}  // namespace

void CheckDailyPractice(const char* url) {
  auto pool = std::make_shared<DatabaseConnectionPool>(url, 4);
  AttemptRepository repository(pool);
  AttemptRepository second_repository(pool);
  std::unique_ptr<PGconn, decltype(&PQfinish)> db(PQconnectdb(url), PQfinish);
  Check(db && PQstatus(db.get()) == CONNECTION_OK, "daily test database unavailable");
  const auto base = -15'000'000'000 - std::chrono::steady_clock::now().time_since_epoch().count() % 1'000'000'000;

  const auto initial = repository.LoadDailyPractice(base, "Europe/Berlin");
  Check(initial.timezone == "Europe/Berlin" && initial.local_day.size() == 10 && !initial.next_rollover.empty() &&
            !initial.session_id && !initial.practiced && !initial.completed && !initial.closed,
        "daily opening created practice or omitted the day contract");
  Check(BooleanQuery(db.get(),
                     "SELECT NOT EXISTS (SELECT 1 FROM daily_practice_assignments WHERE user_id = $1) "
                     "AND NOT EXISTS (SELECT 1 FROM sessions WHERE user_id = $1);",
                     {std::to_string(base)}),
        "daily opening created an assignment or session");
  Check(repository.LoadDailyPractice(base, "Asia/Tokyo").timezone == "Europe/Berlin" &&
            repository.LoadDailyPractice(base).timezone == "Europe/Berlin",
        "browser timezone changed the pinned daily timezone");
  for (const auto& zone : std::vector<std::string>{" Europe/Berlin", "Europe/Berlin ", "Europe/Unknown", "CET",
                                                   "Europe/Berlin\n", std::string(129, 'x'), "UTC+3", "../UTC"}) {
    bool rejected = false;
    try {
      static_cast<void>(repository.LoadDailyPractice(base, zone));
    } catch (const std::invalid_argument&) {
      rejected = true;
    }
    Check(rejected, "invalid daily timezone was accepted");
  }
  Check(repository.LoadDailyPractice(base - 1).timezone == "UTC", "missing timezone did not default to UTC");

  const auto ordinary = repository.StartActiveTrainingSession(base, "miniapp:ordinary", "test-v1", "{}");
  std::promise<void> start;
  const auto signal = start.get_future().share();
  auto first = std::async(std::launch::async, [&] {
    signal.wait();
    return repository.StartActiveTrainingSession(base, "miniapp:daily", "test-v1", "{}", {},
                                                 DailyPracticeStart{initial.local_day, "daily-card-a", "due_review"});
  });
  auto second = std::async(std::launch::async, [&] {
    signal.wait();
    return second_repository.StartActiveTrainingSession(
        base, "miniapp:daily", "test-v1", "{}", {}, DailyPracticeStart{initial.local_day, "daily-card-b", "recovery"});
  });
  start.set_value();
  const auto session_id = first.get();
  Check(second.get() == session_id, "concurrent daily starts created different sessions");
  auto daily = repository.LoadDailyPractice(base);
  Check(daily.session_id == session_id && daily.content_version == "test-v1" && !daily.practiced && !daily.completed &&
            ((daily.question_id == "daily-card-a" && daily.selection_reason == "due_review") ||
             (daily.question_id == "daily-card-b" && daily.selection_reason == "recovery")),
        "concurrent daily start did not persist one complete selection");
  Check(BooleanQuery(db.get(),
                     "SELECT (SELECT count(*) FROM daily_practice_assignments WHERE user_id = $1) = 1 "
                     "AND (SELECT count(*) FROM sessions WHERE user_id = $1) = 2;",
                     {std::to_string(base)}),
        "concurrent daily retry left orphan sessions");

  const auto other = repository.StartActiveTrainingSession(base, "miniapp:other", "test-v1", "{}");
  Check(repository.StartActiveTrainingSession(base, "miniapp:daily", "test-v1", "{}", {},
                                              DailyPracticeStart{initial.local_day, "replacement", "new_skill"}) ==
                session_id &&
            repository.LoadActiveTrainingSession(base)->session_id == other,
        "daily duplicate replaced its assignment or another active training");
  const auto saved = repository.LoadSavedTrainingSessions(base);
  Check(std::ranges::any_of(saved, [ordinary](const auto& entry) { return entry.session_id == ordinary; }),
        "starting daily discarded the earlier ordinary training");
  const auto parked = std::ranges::find(saved, session_id, &ActiveTrainingSessionRecord::session_id);
  Check(parked != saved.end(), "daily snapshot was not preserved when another training started");
  const auto version = repository.ResumeTrainingSession(base, session_id, parked->state_version,
                                                        parked->content_version, parked->state_json);
  Check(repository.LoadDailyPractice(base).session_id == session_id, "daily resume changed assignment identity");
  SubmissionRecord submission{
      .user_id = base,
      .session_id = session_id,
      .question_id = daily.question_id,
      .topic = "integration-daily-topic",
      .skill_id = "integration.daily-skill",
      .question_family_id = "integration.daily-family",
      .evidence_type = QuestionEvidenceType::kApplication,
      .answer = "synthetic wrong answer",
      .correct = false,
      .completed = false,
      .attempt_number = 1,
  };
  static_cast<void>(repository.RecordSubmission(submission));
  daily = second_repository.LoadDailyPractice(base);
  Check(daily.practiced && !daily.completed && !daily.closed,
        "wrong answer did not count as practice or incorrectly completed daily");
  submission.answer = "synthetic assisted answer";
  submission.correct = true;
  submission.completed = true;
  submission.assisted = true;
  submission.attempt_number = 2;
  static_cast<void>(repository.RecordSubmission(submission));
  Check(repository.LoadCleanQuestionFamilies(base).empty(), "daily assisted answer created independent evidence");
  const auto receipt = repository.CompleteActiveTrainingSession(
      base, session_id, version, 2, [](const UserProgressSummary&) { return R"({"status":"completed"})"; });
  Check(repository.CompleteActiveTrainingSession(base, session_id, version, 2,
                                                 [](const UserProgressSummary&) {
                                                   throw std::runtime_error("receipt retry rebuilt result");
                                                   return "";
                                                 }) == receipt,
        "completion retry did not preserve the daily receipt");
  daily = repository.LoadDailyPractice(base);
  Check(daily.practiced && daily.completed && !daily.closed && daily.session_id == session_id,
        "daily completion was lost after deleting the finished snapshot");
  Check(repository.StartActiveTrainingSession(base, "miniapp:daily", "test-v1", "{}", {},
                                              DailyPracticeStart{initial.local_day, "replacement", "new_skill"}) ==
            session_id,
        "completed daily was assigned a second session");

  const auto stop_user = base - 1;
  const auto stop_day = repository.LoadDailyPractice(stop_user);
  const auto stopped = repository.StartActiveTrainingSession(
      stop_user, "miniapp:daily", "test-v1", "{}", {},
      DailyPracticeStart{stop_day.local_day, "daily-stopped", "general_practice"});
  repository.FinishSession(stopped, "miniapp_stopped");
  const auto closed = repository.LoadDailyPractice(stop_user);
  Check(closed.closed && !closed.completed && !closed.practiced && closed.session_id == stopped,
        "stopping daily fabricated practice or completion");
  Check(repository.StartActiveTrainingSession(stop_user, "miniapp:daily", "test-v1", "{}", {},
                                              DailyPracticeStart{stop_day.local_day, "replacement", "new_skill"}) ==
            stopped,
        "stopped daily was assigned a second session");

  const auto stale_user = base - 2;
  static_cast<void>(repository.LoadDailyPractice(stale_user));
  const auto retained = repository.StartActiveTrainingSession(stale_user, "miniapp:ordinary", "test-v1", "{}");
  bool stale_rejected = false;
  try {
    static_cast<void>(
        repository.StartActiveTrainingSession(stale_user, "miniapp:daily", "test-v1", "{}", {},
                                              DailyPracticeStart{"1900-01-01", "daily-stale", "due_review"}));
  } catch (const std::logic_error&) {
    stale_rejected = true;
  }
  Check(stale_rejected && repository.LoadActiveTrainingSession(stale_user)->session_id == retained &&
            !repository.LoadDailyPractice(stale_user).session_id,
        "stale day changed the active session or assigned another day");

  // Controlled SQL instants test the actual PostgreSQL calendar expressions;
  // the public repository/API deliberately has no client-controlled clock.
  Check(BooleanQuery(db.get(), R"sql(
SELECT
  (('2026-03-30'::DATE)::TIMESTAMP AT TIME ZONE 'Europe/Berlin') -
  (('2026-03-29'::DATE)::TIMESTAMP AT TIME ZONE 'Europe/Berlin') = INTERVAL '23 hours'
  AND (('2026-10-26'::DATE)::TIMESTAMP AT TIME ZONE 'Europe/Berlin') -
  (('2026-10-25'::DATE)::TIMESTAMP AT TIME ZONE 'Europe/Berlin') = INTERVAL '25 hours'
  AND ('2026-11-05 20:59:59+00'::TIMESTAMPTZ AT TIME ZONE 'Europe/Moscow')::DATE = DATE '2026-11-05'
  AND ('2026-11-05 21:00:00+00'::TIMESTAMPTZ AT TIME ZONE 'Europe/Moscow')::DATE = DATE '2026-11-06'
  AND ('2026-10-25 00:30:00+00'::TIMESTAMPTZ AT TIME ZONE 'Europe/Berlin')::DATE =
      ('2026-10-25 01:30:00+00'::TIMESTAMPTZ AT TIME ZONE 'Europe/Berlin')::DATE;
)sql"),
        "daily DST or midnight calendar contract is incorrect");
}
