#include "app/education_direction.h"
#include "miniapp/miniapp_service.h"
#include "storage/attempt_repository.h"
#include "storage/question_loader.h"
#include "system_design/catalog.h"
#include "system_design/service.h"

#include <libpq-fe.h>

#include <chrono>
#include <future>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

namespace {

void Require(bool condition, std::string_view message) {
  if (!condition)
    throw std::runtime_error(std::string(message));
}

template <typename Error, typename Operation>
void Denied(Operation&& operation) {
  bool denied = false;
  try {
    operation();
  } catch (const Error&) {
    denied = true;
  }
  Require(denied, "expected System Design operation rejection");
}

std::int64_t Number(const boost::json::object& object, std::string_view key) {
  const auto& value = object.at(key);
  return value.is_int64() ? value.as_int64() : static_cast<std::int64_t>(value.as_uint64());
}

std::string Text(const boost::json::object& object, std::string_view key) {
  return std::string(object.at(key).as_string());
}

}  // namespace

void CheckSystemDesign(const char* url) {
  using boost::json::object;
  const auto uid = 15'000'000'000 + std::chrono::steady_clock::now().time_since_epoch().count() % 1'000'000'000;
  auto pool = std::make_shared<DatabaseConnectionPool>(url, 3);
  std::unique_ptr<PGconn, decltype(&PQfinish)> connection(PQconnectdb(url), PQfinish);
  Require(connection && PQstatus(connection.get()) == CONNECTION_OK, "SD test database unavailable");
  const auto sql = [&](const std::string& query) -> std::string {
    std::unique_ptr<PGresult, decltype(&PQclear)> result(PQexec(connection.get(), query.c_str()), PQclear);
    Require(result && PQresultStatus(result.get()) == PGRES_TUPLES_OK, "SD fixture query failed");
    return PQgetvalue(result.get(), 0, 0);
  };
  const auto cppSnapshot = [&] {
    std::string snapshot;
    for (const auto table : {"users", "sessions", "attempts", "question_progress", "topic_progress", "skill_progress",
                             "skill_evidence", "user_skill_state", "xp_ledger", "active_training_sessions",
                             "saved_training_sessions", "daily_practice_assignments", "user_daily_settings"})
      snapshot += sql("SELECT COALESCE(jsonb_agg(to_jsonb(t) ORDER BY to_jsonb(t)::text),'[]')::text FROM " +
                      std::string(table) + " t WHERE user_id=" + std::to_string(uid));
    return snapshot;
  };
  QuestionBank bank(LoadQuestions("content/questions"));
  MiniAppService cpp(bank, pool, uid);
  static_cast<void>(cpp.Bootstrap(
      TelegramMiniAppUser{.id = uid, .first_name = {}, .last_name = {}, .username = {}, .language_code = {}}));
  const auto cpp_session = cpp.StartSession(uid, {{"recommended", true}});
  AttemptRepository cpp_repository(pool);
  const auto cpp_saved = cpp_repository.LoadActiveTrainingSession(uid);
  const auto before = cppSnapshot();

  Require(AvailableEducationDirections(uid, uid).size() == 2 &&
              AvailableEducationDirections(uid + 1, uid).size() == 1 &&
              AvailableEducationDirections(uid, 0).size() == 1,
          "direction availability is not owner-only");
  system_design::Service service(pool, uid), disabled(pool, 0), invalid(pool, -1), other(pool, uid + 1);
  for (const auto action : {"bootstrap", "module", "complete-module", "start", "current", "session", "save", "hint",
                            "advance", "self-review"}) {
    Denied<system_design::AccessDenied>(
        [&] { static_cast<void>(service.Handle(uid + 1, action, {{"user_id", uid}})); });
    Denied<system_design::AccessDenied>([&] { static_cast<void>(disabled.Handle(uid, action, {})); });
    Denied<system_design::AccessDenied>([&] { static_cast<void>(invalid.Handle(uid, action, {})); });
  }
  const auto& module = system_design::InterviewModule();
  const auto& exercise = system_design::UrlShortenerCase();
  Require(module.sections.size() >= 6 && exercise.stages.size() == 12, "SD curriculum incomplete");
  for (const auto& stage : exercise.stages) {
    Require(!stage.id.empty() && !stage.prompt.empty() && stage.hints.size() == 2 && stage.criteria.size() == 3 &&
                !stage.debrief.empty() && !stage.alternatives.empty(),
            "SD stage lacks pedagogical structure");
    for (const auto& source : stage.sources)
      Require(source.url.starts_with("https://"), "SD source must use HTTPS");
  }
  std::size_t command = 0;
  const auto nextId = [&] {
    return "sd01-command-request-" + std::to_string(++command);
  };
  object start{{"case_id", exercise.id}, {"request_id", nextId()}};
  Denied<std::logic_error>([&] { static_cast<void>(service.Handle(uid, "start", start)); });
  Denied<std::invalid_argument>([&] { static_cast<void>(service.Handle(uid, "complete-module", {})); });
  Require(!service.Handle(uid, "bootstrap", {}).at("module").as_object().at("completed").as_bool(),
          "module pre-completed");
  static_cast<void>(service.Handle(uid, "module", {}));
  static_cast<void>(service.Handle(uid, "complete-module", {{"acknowledged", true}}));
  static_cast<void>(service.Handle(uid, "complete-module", {{"acknowledged", true}}));
  auto session = service.Handle(uid, "start", start);
  const auto id = Number(session, "session_id");
  Require(service.Handle(uid, "start", start) == session, "start retry changed the SD session");
  auto another_start = start;
  another_start["request_id"] = nextId();
  Require(Number(service.Handle(uid, "start", another_start), "session_id") == id, "active case was replaced");
  Require(session.at("stage").as_object().at("hints").as_array().empty() && !session.contains("review"),
          "SD solution leaked at start");
  Denied<std::logic_error>([&] { static_cast<void>(other.Handle(uid + 1, "session", {{"session_id", id}})); });
  const auto request = [&](std::string_view text = {}) {
    return object{{"session_id", id},
                  {"version", Number(session, "version")},
                  {"request_id", nextId()},
                  {"stage_id", session.at("stage").as_object().at("id")},
                  {"text", text}};
  };
  const std::string own_text = "Мои вопросы и допущения\n'; DROP TABLE attempts; -- <script>notExecuted</script>";
  auto save = request(own_text);
  session = service.Handle(uid, "save", save);
  const auto saved_version = Number(session, "version");
  Require(Text(session.at("stage").as_object(), "draft") == own_text, "draft changed in storage");
  Require(service.Handle(uid, "save", save) == session, "save replay was applied twice");
  auto changed = save;
  changed["text"] = "different";
  Denied<std::invalid_argument>([&] { static_cast<void>(service.Handle(uid, "save", changed)); });
  changed["request_id"] = nextId();
  Denied<std::logic_error>([&] { static_cast<void>(service.Handle(uid, "save", changed)); });
  auto excessive = request(std::string(16001, 'x'));
  Denied<std::invalid_argument>([&] { static_cast<void>(service.Handle(uid, "save", excessive)); });
  auto blank = request(" \n\t");
  Denied<std::invalid_argument>([&] { static_cast<void>(service.Handle(uid, "advance", blank)); });
  Require(Number(service.Handle(uid, "session", {{"session_id", id}}), "version") == saved_version,
          "rejected mutation changed the session version");
  auto hint = request();
  session = service.Handle(uid, "hint", hint);
  Require(session.at("stage").as_object().at("hints").as_array().size() == 1, "hint prefix not enforced");
  system_design::Service restarted(pool, uid);
  Require(restarted.Handle(uid, "current", {}).at("session") == session, "SD restart lost draft/hint/version");
  Require(restarted.Handle(uid, "hint", hint) == session, "hint replay revealed another hint");

  // Concurrent old versions cannot silently overwrite one another.
  const auto concurrent_a = request("A: own architecture questions"), concurrent_b = request("B: another draft");
  const auto attempt = [&](const object& body) {
    try {
      static_cast<void>(service.Handle(uid, "save", body));
      return true;
    } catch (const std::logic_error&) {
      return false;
    }
  };
  auto a = std::async(std::launch::async, attempt, concurrent_a);
  auto b = std::async(std::launch::async, attempt, concurrent_b);
  Require(a.get() != b.get(), "two stale saves both overwrote the SD draft");
  session = service.Handle(uid, "session", {{"session_id", id}});
  object final_request;
  for (std::size_t i = 0; i < exercise.stages.size(); ++i) {
    Require(Number(session, "current_stage") == static_cast<std::int64_t>(i), "SD stage order changed");
    Require(!session.contains("review") && boost::json::serialize(session).find("73 млрд байт") == std::string::npos,
            "debrief leaked before completing own approach");
    final_request = request("Свой подход к этапу " + std::to_string(i) + ": ограничения и цена выбора.");
    session = service.Handle(uid, "advance", final_request);
  }
  Require(
      Text(session, "status") == "completed" && session.at("review").as_object().at("stages").as_array().size() == 12,
      "completed case has no saved detailed review");
  Require(Text(session.at("review").as_object(), "assessment_kind") == "self_review_not_automatic",
          "free text was falsely graded");
  Require(service.Handle(uid, "advance", final_request) == session, "completion retry duplicated the result");
  const auto state = service.Handle(uid, "bootstrap", {});
  Require(Number(state.at("statistics").as_object(), "completed") == 1 &&
              Number(state.at("statistics").as_object(), "started") == 1,
          "independent SD statistics wrong");
  object mark{{"session_id", id},       {"version", Number(session, "version")},
              {"request_id", nextId()}, {"stage", 0},
              {"criterion", 0},         {"status", "covered"}};
  session = restarted.Handle(uid, "self-review", mark);
  Require(restarted.Handle(uid, "session", {{"session_id", id}}) == session, "self review not durable");
  Require(Text(session.at("review")
                   .as_object()
                   .at("stages")
                   .as_array()[0]
                   .as_object()
                   .at("criteria")
                   .as_array()[0]
                   .as_object(),
               "status") == "covered",
          "self review missing");
  Require(service.Handle(uid, "current", {}).at("session").is_null(), "completed case remains active");
  Require(service.Handle(uid, "start", start).at("status") == "completed", "lost start response created a new attempt");
  start["request_id"] = nextId();
  Require(Number(service.Handle(uid, "start", start), "session_id") != id, "new attempt did not preserve history");
  Require(service.Handle(uid, "bootstrap", {}).at("history").as_array().size() == 2, "SD attempt history lost");
  Require(cppSnapshot() == before, "System Design changed C++ profile/progress/XP/sessions");
  const auto after = cpp_repository.LoadActiveTrainingSession(uid);
  Require(after && cpp_saved && after->session_id == Number(cpp_session, "session_id") &&
              after->state_json == cpp_saved->state_json && after->state_version == cpp_saved->state_version,
          "SD workflow replaced C++ active training");
  std::cout << "SD-01 service PASS: access/isolation/restart/idempotency/stale saves/history/self-review\n";
}
