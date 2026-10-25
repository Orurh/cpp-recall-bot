#include <libpq-fe.h>
#include <boost/json.hpp>

#include <chrono>
#include <memory>
#include <stdexcept>
#include <string>

namespace {

void Check(bool condition, const char* message) {
  if (!condition)
    throw std::runtime_error(message);
}

std::string Sql(PGconn* connection, const std::string& query) {
  std::unique_ptr<PGresult, decltype(&PQclear)> result(PQexec(connection, query.c_str()), PQclear);
  Check(result && (PQresultStatus(result.get()) == PGRES_TUPLES_OK || PQresultStatus(result.get()) == PGRES_COMMAND_OK),
        "learning statistics SQL fixture failed");
  return PQntuples(result.get()) ? PQgetvalue(result.get(), 0, 0) : "";
}

}  // namespace

void CheckLearningStatistics(const char* url) {
  std::unique_ptr<PGconn, decltype(&PQfinish)> connection(PQconnectdb(url), PQfinish);
  Check(connection && PQstatus(connection.get()) == CONNECTION_OK, "learning statistics database unavailable");
  auto* db = connection.get();
  Sql(db, "BEGIN; SET LOCAL TIME ZONE 'Pacific/Honolulu';");
  try {
    // Fixed historical clock: no sleeps, no dependence on today's UTC boundary.
    const auto statistics = [&](int days = 30, const std::string& cutoff = "2000-03-12T00:00:00Z") {
      return boost::json::parse(Sql(db, "SELECT learning_statistics(" + std::to_string(days) + ", '" + cutoff +
                                            "'::timestamptz)::text"))
          .as_object();
    };
    auto data = statistics();
    Check(data.at("activated_users").as_int64() == 0, "empty historical cohort is not isolated");
    Check(data.at("first_training").as_object().at("completion_rate").is_null(),
          "no first trainings was presented as a measured completion rate");
    for (const auto* name : {"d1", "d7_9"}) {
      const auto& value = data.at("returns").as_object().at(name).as_object();
      Check(value.at("observed").as_int64() == 0 && value.at("eligible").as_int64() == 0 &&
                value.at("returned").as_int64() == 0 && value.at("rate").is_null(),
            "empty learning return was presented as a measured result");
    }
    const auto base = 4'000'000'000 + std::chrono::steady_clock::now().time_since_epoch().count() % 1'000'000'000;
    for (int offset = -1; offset <= 11; ++offset) {
      const auto uid = offset < 0 ? -base : base + offset;
      Sql(db, "INSERT INTO users(user_id) VALUES (" + std::to_string(uid) + ")");
    }
    const auto session = [&](std::int64_t uid) {
      // Synthetic history, not a real service run; do not create now()-dated trigger events.
      return std::stoll(Sql(db, "INSERT INTO sessions(user_id, mode) VALUES (" + std::to_string(uid) +
                                    ", 'statistics-fixture') RETURNING id"));
    };
    int sequence = 0;
    const auto event = [&](std::int64_t uid, const std::string& name, const std::string& time, std::int64_t sid = 0,
                           const std::string& outcome = "", bool correct = true) {
      Sql(db,
          "INSERT INTO learning_events(event_key, event_name, user_id, session_id, occurred_at, outcome, "
          "question_id, attempt_number, correct) VALUES ('learning-fixture:" +
              std::to_string(base) + ":" + std::to_string(++sequence) + "', '" + name + "', " + std::to_string(uid) +
              ", " + (sid ? std::to_string(sid) : "NULL") + ", '" + time + "', '" + outcome + "', 'fixture', " +
              (name == "answer" ? "1" : "NULL") + ", " + (name == "answer" ? correct ? "true" : "false" : "NULL") +
              ")");
    };
    const auto answer = [&](int user, const std::string& time, bool correct = true) {
      const auto uid = base + user;
      event(uid, "answer", time, session(uid), "", correct);
    };
    // One learner returned on three days in the weekly window, but counts only once.
    answer(0, "2000-03-02T12:00:00Z", false);
    answer(0, "2000-03-03T00:00:00Z");
    answer(0, "2000-03-09T00:00:00Z");
    answer(0, "2000-03-10T12:00:00Z");
    answer(0, "2000-03-11T23:59:59.999Z");
    answer(1, "2000-03-02T12:00:00Z");
    answer(1, "2000-03-04T12:00:00Z");      // Day +2 is not D1.
    answer(1, "2000-03-08T23:59:59.999Z");  // Day +6 is outside D7–D9.
    answer(1, "2000-03-12T00:00:00Z");      // Day +10 and at the observation cutoff.
    answer(2, "2000-03-03T12:00:00Z");
    answer(2, "2000-03-10T12:00:00Z");  // A return before the full window matures must not enter its numerator.
    answer(3, "2000-03-10T23:59:59.999Z");
    answer(3, "2000-03-11T00:00:00Z");  // Calendar D1, even one millisecond later.
    answer(4, "2000-03-11T12:00:00Z", false);
    answer(4, "2000-03-11T23:59:59Z", false);  // Wrong-only activity still activates; same-day repeat is not a return.
    answer(5, "2000-03-12T00:00:00Z");         // Future relative to this report, not an activation.
    answer(6, "2000-02-01T12:00:00Z");
    answer(6, "2000-03-11T12:00:00Z");  // An old learner must not become newly activated again.
    event(base + 7, "app_active", "2000-03-02T12:00:00Z");
    event(base + 7, "app_active", "2000-03-03T12:00:00Z");
    event(base + 7, "app_active", "2000-03-11T12:00:00Z");  // Opening alone is not learning.
    event(-base, "answer", "2000-03-02T12:00:00Z");         // Demo history is excluded even if present.

    const auto stopped = session(base + 8);
    event(base + 8, "training_started", "2000-03-03T12:00:00Z", stopped);
    event(base + 8, "training_finished", "2000-03-03T13:00:00Z", stopped, "miniapp_stopped");
    const auto later = session(base + 8);
    event(base + 8, "training_started", "2000-03-04T12:00:00Z", later);
    event(base + 8, "answer", "2000-03-04T12:01:00Z", later);
    event(base + 8, "training_finished", "2000-03-04T13:00:00Z", later, "completed");
    // A completed later session must not repair the first-session completion funnel.
    const auto complete = session(base + 9);
    event(base + 9, "training_started", "2000-03-04T12:00:00Z", complete);
    event(base + 9, "answer", "2000-03-04T12:01:00Z", complete, "", false);
    event(base + 9, "answer", "2000-03-04T12:02:00Z", complete);
    event(base + 9, "training_finished", "2000-03-04T13:00:00Z", complete, "completed");
    const auto old = session(base + 10);
    event(base + 10, "training_started", "2000-02-01T12:00:00Z", old);
    event(base + 10, "answer", "2000-02-01T12:01:00Z", old);
    event(base + 10, "training_started", "2000-03-11T12:00:00Z", session(base + 10));
    const auto active = session(base + 11);
    event(base + 11, "training_started", "2000-03-11T12:00:00Z", active);
    event(base + 11, "training_finished", "2000-03-12T00:00:00Z", active, "completed");

    data = statistics();
    Check(data.at("rules_version").as_int64() == 1 && data.at("timezone").as_string() == "UTC" &&
              data.at("cohort_since").as_string() == "2000-02-12",
          "statistics rules or UTC period shifted with the database timezone");
    Check(data.at("activated_users").as_int64() == 7, "activation included old, future, duplicate or app-only users");
    const auto& first = data.at("first_training").as_object();
    Check(first.at("started_users").as_int64() == 3 && first.at("answered_users").as_int64() == 1 &&
              first.at("completed_users").as_int64() == 1 && first.at("completion_rate").as_double() == 33.3,
          "first-training funnel included later trainings, future completions or old starts");
    const auto& d1 = data.at("returns").as_object().at("d1").as_object();
    Check(d1.at("observed").as_int64() == 7 && d1.at("eligible").as_int64() == 6 && d1.at("pending").as_int64() == 1 &&
              d1.at("returned").as_int64() == 2 && d1.at("rate").as_double() == 33.3,
          "calendar D1 mixed app visits, immature learners or same-day repeats");
    const auto& weekly = data.at("returns").as_object().at("d7_9").as_object();
    Check(weekly.at("observed").as_int64() == 7 && weekly.at("eligible").as_int64() == 2 &&
              weekly.at("pending").as_int64() == 5 && weekly.at("returned").as_int64() == 1 &&
              weekly.at("rate").as_double() == 50.0,
          "weekly learning return counted duplicate days, an unfinished window or day +6/+10");
    const auto after_window = statistics(30, "2000-03-13T00:00:00Z");
    const auto& after_weekly = after_window.at("returns").as_object().at("d7_9").as_object();
    Check(after_weekly.at("eligible").as_int64() == 3 && after_weekly.at("returned").as_int64() == 2,
          "a day +10 answer became a weekly return once it was in the past");
    const auto short_period = statistics(7);
    Check(short_period.at("activated_users").as_int64() == 2 &&
              short_period.at("returns").as_object().at("d7_9").as_object().at("rate").is_null(),
          "narrowing the report period reset an existing learner's first answer");
    const auto before_boundary = statistics(30, "2000-03-11T23:59:59.999Z");
    Check(before_boundary.at("returns").as_object().at("d7_9").as_object().at("eligible").as_int64() == 0 &&
              before_boundary.at("returns").as_object().at("d7_9").as_object().at("returned").as_int64() == 0 &&
              before_boundary.at("returns").as_object().at("d7_9").as_object().at("rate").is_null(),
          "cohort matured before its entire UTC observation window ended");
    Sql(db, "SET LOCAL TIME ZONE 'UTC';");
    Check(statistics() == data, "server timezone changed the UTC learning statistics");
    Sql(db, "ROLLBACK;");
  } catch (...) {
    static_cast<void>(Sql(db, "ROLLBACK;"));
    throw;
  }
}
