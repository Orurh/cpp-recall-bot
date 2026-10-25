#include "learning/theory_catalog.h"
#include "miniapp/miniapp_service.h"
#include "storage/attempt_repository.h"
#include "storage/question_loader.h"

#include <libpq-fe.h>
#include <boost/json.hpp>

#include <algorithm>
#include <chrono>
#include <future>
#include <memory>
#include <stdexcept>
#include <string>

namespace {

void Check(bool condition, const char* message) {
  if (!condition)
    throw std::runtime_error(message);
}

std::string Sql(PGconn* connection, const std::string& sql) {
  std::unique_ptr<PGresult, decltype(&PQclear)> result(PQexec(connection, sql.c_str()), PQclear);
  if (!result ||
      (PQresultStatus(result.get()) != PGRES_TUPLES_OK && PQresultStatus(result.get()) != PGRES_COMMAND_OK)) {
    throw std::runtime_error(std::string("statistics test SQL failed: ") + PQerrorMessage(connection));
  }
  return PQntuples(result.get()) ? PQgetvalue(result.get(), 0, 0) : "";
}

std::int64_t Count(PGconn* connection, const std::string& predicate) {
  return std::stoll(Sql(connection, "SELECT count(*) FROM learning_events WHERE " + predicate));
}

}  // namespace

void CheckBetaStatistics(const char* url) {
  std::unique_ptr<PGconn, decltype(&PQfinish)> connection(PQconnectdb(url), PQfinish);
  Check(connection && PQstatus(connection.get()) == CONNECTION_OK, "statistics test database unavailable");
  auto* db = connection.get();
  auto pool = std::make_shared<DatabaseConnectionPool>(url, 3);
  AttemptRepository repository(pool);
  Check(repository.CheckReady() && pool->Available() == 3, "readiness leaked a pooled connection");
  Sql(db, "BEGIN; LOCK TABLE schema_migrations IN ACCESS EXCLUSIVE MODE;");
  const auto readiness_start = std::chrono::steady_clock::now();
  const bool blocked_ready = repository.CheckReady();
  const auto readiness_elapsed = std::chrono::steady_clock::now() - readiness_start;
  Sql(db, "ROLLBACK;");
  Check(!blocked_ready && readiness_elapsed < std::chrono::seconds(3) && repository.CheckReady(),
        "blocked readiness hung or left its connection unusable");
  auto questions = LoadQuestions("content/questions");
  std::erase_if(questions, [](const Question& q) {
    return q.level != 2 || !std::holds_alternative<SingleChoiceTask>(q.task) ||
           FindTheoryForSkill(q.skill_id) == nullptr;
  });
  Check(questions.size() >= 5, "statistics fixture needs five cards with a theory block");
  QuestionBank bank(std::move(questions));
  const auto uid = 2'000'000'000 + std::chrono::steady_clock::now().time_since_epoch().count() % 1'000'000'000;
  const auto owner = uid + 1;
  const auto filter = "user_id = " + std::to_string(uid);
  MiniAppService service(bank, pool, owner);
  MiniAppService other(bank, pool, owner);
  bool denied = false;
  try {
    static_cast<void>(service.Statistics(uid, {{"days", 30}}));
  } catch (const FeedbackAccessDenied&) {
    denied = true;
  }
  Check(denied, "statistics exposed to a regular user");
  repository.RecordBotStart(uid, 1);
  repository.RecordBotStart(uid, 1);
  Check(Count(db, filter + " AND event_name = 'bot_start'") == 1, "bot start was duplicated");

  const boost::json::object start{{"modes", boost::json::array{"theory", "api", "code"}},
                                  {"topics", boost::json::array{}},
                                  {"level", 2},
                                  {"request_id", "20000000-0000-4000-8000-000000000001"}};
  auto first = std::async(std::launch::async, [&] { return service.StartSession(uid, start); });
  auto second = std::async(std::launch::async, [&] { return other.StartSession(uid, start); });
  auto session = first.get();
  Check(session == second.get(), "concurrent session retries returned different sessions");
  Check(Count(db, filter + " AND event_name = 'training_started'") == 1, "session start was duplicated");
  const auto sid = *repository.FindSessionByRequest(uid, "20000000-0000-4000-8000-000000000001");
  Check(Count(db, filter + " AND event_name = 'card_shown' AND content_revision <> ''") == 1,
        "initial presentation or content revision was not recorded");

  boost::json::object pulse{{"visit_id", "30000000-0000-4000-8000-000000000001"},
                            {"sequence", 1},
                            {"visible", true},
                            {"context", "training"}};
  Check(service.RecordActivity(uid, pulse).at("accepted").as_bool(), "initial activity was rejected");
  Check(!service.RecordActivity(uid, pulse).at("accepted").as_bool(), "duplicate activity was accepted");
  Sql(db, "UPDATE analytics_presence SET last_seen_at = now() - interval '4 seconds' WHERE " + filter);
  pulse["sequence"] = 2;
  static_cast<void>(service.RecordActivity(uid, pulse));
  Check(std::stoll(Sql(db, "SELECT sum(active_ms) FROM activity_samples WHERE " + filter)) >= 3900,
        "active learning time was not counted");
  Sql(db, "UPDATE analytics_presence SET last_seen_at = now() - interval '2 hours' WHERE " + filter);
  pulse["sequence"] = 3;
  static_cast<void>(service.RecordActivity(uid, pulse));
  Check(Sql(db, "SELECT active_ms FROM activity_samples WHERE " + filter + " AND sequence = 3") == "0",
        "background hours were counted as study");
  pulse["sequence"] = 2;
  Check(!service.RecordActivity(uid, pulse).at("accepted").as_bool(), "out of order pulse changed activity");
  Sql(db, "UPDATE analytics_presence SET last_seen_at = now() - interval '4 seconds' WHERE " + filter);
  pulse["visit_id"] = "30000000-0000-4000-8000-000000000002";
  pulse["sequence"] = 1;
  static_cast<void>(service.RecordActivity(uid, pulse));
  Check(Sql(db, "SELECT active_ms FROM activity_samples WHERE " + filter + " ORDER BY observed_at DESC LIMIT 1") == "0",
        "switching tabs duplicated active time");
  Check(Count(db, filter + " AND event_name = 'session_resumed'") == 2, "session resumes were not deduplicated");

  const auto qid = std::string(session.at("question").as_object().at("id").as_string());
  const auto& task = std::get<SingleChoiceTask>(bank.Get(qid).task);
  const auto wrong =
      std::ranges::find_if(task.options, [&](const auto& item) { return item.id != task.correct_option_id; });
  const boost::json::object answer{{"question_id", qid}, {"answer", wrong->id}};
  Sql(db,
      "CREATE OR REPLACE FUNCTION beta_test_fail_progress() RETURNS trigger LANGUAGE plpgsql AS $$ BEGIN "
      "IF NEW.user_id = " +
          std::to_string(uid) +
          " THEN RAISE EXCEPTION 'test rollback'; END IF; RETURN NEW; END; $$; "
          "CREATE TRIGGER beta_test_fail_progress BEFORE UPDATE ON active_training_sessions "
          "FOR EACH ROW EXECUTE FUNCTION beta_test_fail_progress();");
  bool failed = false;
  try {
    static_cast<void>(service.SubmitAnswer(uid, answer));
  } catch (const std::runtime_error&) {
    failed = true;
  }
  Sql(db, "DROP TRIGGER beta_test_fail_progress ON active_training_sessions; DROP FUNCTION beta_test_fail_progress();");
  Check(failed && Count(db, filter + " AND event_name = 'answer'") == 0 &&
            Sql(db, "SELECT count(*) FROM attempts WHERE " + filter) == "0",
        "a failed submission left an orphan event or attempt");
  static_cast<void>(service.SubmitAnswer(uid, answer));
  static_cast<void>(service.SubmitAnswer(uid, answer));
  Check(Count(db, filter + " AND event_name = 'answer' AND practice_kind = 'new' AND NOT correct") == 1,
        "answer retry was duplicated or lost practice type");
  static_cast<void>(service.OpenTheory(uid, {{"question_id", qid}}));
  static_cast<void>(service.OpenTheory(uid, {{"question_id", qid}}));
  Check(Count(db, filter + " AND event_name = 'theory_opened'") == 1, "theory opens were lost or duplicated");

  // Finish all primary cards and the repair tail through the real service.
  session = service.NextQuestion(uid);
  for (int limit = 0; limit < 30 && session.at("status") != "completed"; ++limit) {
    const auto id = std::string(session.at("question").as_object().at("id").as_string());
    const auto& choice = std::get<SingleChoiceTask>(bank.Get(id).task);
    static_cast<void>(service.SubmitAnswer(uid, {{"question_id", id}, {"answer", choice.correct_option_id}}));
    session = service.NextQuestion(uid);
  }
  Check(session.at("status") == "completed", "test training did not complete");
  repository.FinishSession(sid, "completed");
  Check(Count(db, filter + " AND event_name = 'training_finished' AND outcome = 'completed'") == 1,
        "completion was not atomic and idempotent");
  Check(Count(db, filter + " AND event_name = 'answer' AND practice_kind = 'repair' AND correct AND assisted") == 1,
        "assisted correction was not distinguished from new learning");

  Sql(db, "UPDATE analytics_visits SET opened_at = now() - interval '8 days' WHERE " + filter);
  Sql(db,
      "UPDATE learning_events SET occurred_at = date_trunc('day', now() AT TIME ZONE 'UTC') AT TIME ZONE 'UTC' - "
      "interval '7 days' WHERE " +
          filter + " AND event_name = 'app_active'");
  Sql(db, "INSERT INTO learning_events(event_key, event_name, user_id, occurred_at) VALUES ('test-d7-" +
              std::to_string(uid) + "', 'app_active', " + std::to_string(uid) + ", now() - interval '1 day')");
  auto stats = service.Statistics(owner, {{"days", 30}});
  Check(stats.at("returns").as_object().at("d1_returned").as_int64() >= 1, "mature D1 cohort was not counted");
  Check(stats.at("returns").as_object().at("d7_returned").as_int64() >= 1, "mature D7 cohort was not counted");
  Check(stats.at("summary").as_object().at("completed").as_int64() >= 1, "completed training missing from report");
  const auto& learning = stats.at("learning").as_object();
  Check(learning.at("activated_users").as_int64() >= 1 &&
            learning.at("first_training").as_object().at("completed_users").as_int64() >= 1,
        "real service answers or first training were absent from the learning report");
  const auto product_metrics = [&] {
    auto metrics = boost::json::parse(repository.BetaStatistics(30)).as_object();
    // Only the observation timestamp advances between reports; all counts must stay identical.
    metrics.at("learning").as_object().erase("observed_at");
    return metrics;
  };
  const auto before_demo = product_metrics();
  repository.RecordBotStart(-uid, 1);
  static_cast<void>(service.StartSession(-uid, start));
  static_cast<void>(repository.RecordActivity(-uid, "30000000-0000-4000-8000-000000000003", 1, 0, "", "home", ""));
  static_cast<void>(service.StopSession(-uid));
  Check(Count(db, "user_id = " + std::to_string(-uid)) == 0, "demo events polluted learning statistics");
  Check(product_metrics() == before_demo, "demo account changed product metrics");

  auto another = start;
  another["request_id"] = "20000000-0000-4000-8000-000000000002";
  static_cast<void>(service.StartSession(uid, another));
  static_cast<void>(service.StopSession(uid));
  Check(Count(db, filter + " AND event_name = 'training_finished' AND outcome = 'miniapp_stopped'") == 1,
        "manual stop was reported as completion");
  another["request_id"] = "20000000-0000-4000-8000-000000000003";
  static_cast<void>(service.StartSession(uid, another));
  Sql(db, "UPDATE active_training_sessions SET expires_at = now() - interval '1 second' WHERE " + filter);
  stats = service.Statistics(owner, {{"days", 30}});
  Check(stats.at("summary").as_object().at("expired").as_int64() >= 1, "expired sessions were counted as active");
  another["request_id"] = "20000000-0000-4000-8000-000000000004";
  static_cast<void>(other.StartSession(uid, another));
  Check(Count(db, filter + " AND event_name = 'training_finished' AND outcome = 'expired'") == 1,
        "restarting an expired session erased its timeout outcome");
  static_cast<void>(other.StopSession(uid));
}
