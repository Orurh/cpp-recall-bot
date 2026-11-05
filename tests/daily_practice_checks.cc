#include "learning/training_recommendation.h"
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
void Sql(const char* url, const std::string& statement) {
  std::unique_ptr<PGconn, decltype(&PQfinish)> connection(PQconnectdb(url), PQfinish);
  Check(connection && PQstatus(connection.get()) == CONNECTION_OK, "daily calendar fixture database unavailable");
  std::unique_ptr<PGresult, decltype(&PQclear)> result(PQexec(connection.get(), statement.c_str()), PQclear);
  Check(result && PQresultStatus(result.get()) == PGRES_COMMAND_OK, "daily calendar fixture SQL failed");
}
std::string Text(const boost::json::object& value, std::string_view key) {
  return std::string(value.at(key).as_string());
}
template <typename F>
void Reject(F&& operation) {
  bool rejected = false;
  try {
    operation();
  } catch (const std::logic_error&) {
    rejected = true;
  }
  Check(rejected, "invalid daily service request was accepted");
}
}  // namespace

void CheckDailyPracticeService(const char* url) {
  auto loaded = LoadQuestions("content/questions");
  std::erase_if(loaded, [](const Question& question) { return question.id != "block_scope_destruction_order"; });
  QuestionBank bank(std::move(loaded));
  QuestionBank full(LoadQuestions("content/questions"));
  auto pool = std::make_shared<DatabaseConnectionPool>(url, 4);
  AttemptRepository repository(pool);
  MiniAppService first(bank, pool), second(bank, pool);
  MiniAppService ordinary(full, pool);
  const auto uid = -4'000'000'000 - std::chrono::steady_clock::now().time_since_epoch().count() % 1'000'000'000;
  const TelegramMiniAppUser user{.id = uid, .first_name = {}, .last_name = {}, .username = {}, .language_code = {}};
  Check(first.Bootstrap(user).at("session").is_null(), "bootstrap unexpectedly created a session");
  const auto day = first.DailyPractice(uid, {{"timezone", "Europe/Moscow"}});
  Check(Text(day, "status") == "ready" && !day.at("practiced").as_bool() && day.at("session_id").is_null(),
        "daily preview was credited as practice");
  Check(second.DailyPractice(uid, {{"timezone", "America/New_York"}}) == day, "reopening changed pinned daily zone");
  Reject([&] { static_cast<void>(first.StartSession(uid, {{"daily", true}, {"daily_day", "2000-01-01"}})); });
  Reject([&] {
    static_cast<void>(first.StartSession(uid, {{"daily", true}, {"daily_day", day.at("local_day")}, {"level", 10}}));
  });
  const boost::json::object start{{"daily", true}, {"daily_day", day.at("local_day")}};
  const auto ordinary_before = first.StartSession(uid, {{"recommended", true}});
  auto a = std::async(std::launch::async, [&] { return first.StartSession(uid, start); });
  auto b = std::async(std::launch::async, [&] { return second.StartSession(uid, start); });
  auto session = a.get();
  Check(b.get() == session, "concurrent services created different daily assignments");
  const auto id = session.at("session_id").as_int64();
  Check(session.at("progress").as_object().at("session_goal").as_uint64() == 1 &&
            session.at("daily_day") == day.at("local_day"),
        "daily plan was not exactly one durable question");
  const auto question = session.at("question").as_object();
  const auto& original = bank.Get(Text(question, "id"));
  const auto& task = std::get<SingleChoiceTask>(original.task);
  const auto wrong =
      std::ranges::find_if(task.options, [&](const auto& option) { return option.id != task.correct_option_id; });
  Check(wrong != task.options.end(), "daily test needs a wrong choice");
  static_cast<void>(first.SubmitAnswer(uid, {{"session_id", id}, {"question_id", original.id}, {"answer", wrong->id}}));
  const auto practiced = second.DailyPractice(uid, {});
  Check(practiced.at("practiced").as_bool() && !practiced.at("completed").as_bool(),
        "wrong accepted answer did not preserve practice separately from completion");
  // second still caches the initial question, while first has feedback and
  // switches the authoritative pointer away. Daily start must consult the DB.
  static_cast<void>(ordinary.StartSession(uid, {{"recommended", true}}));
  const auto cross_process = second.StartSession(uid, start);
  Check(Text(cross_process, "phase") == "feedback" && repository.LoadActiveTrainingSession(uid)->session_id == id,
        "stale service cache bypassed daily resume or lost accepted feedback");
  Check(Text(first.StopSession(uid, {{"session_id", id}}), "status") == "paused", "daily stop destroyed its snapshot");
  MiniAppService restored(bank, pool);
  session = restored.StartSession(uid, start);
  Check(Text(session, "phase") == "feedback", "restart lost wrong-answer feedback");
  session = restored.NextQuestion(uid);
  const auto& references = session.at("question").as_object().at("references").as_array();
  Check(!references.empty(), "daily fixture needs before-answer help");
  static_cast<void>(restored.OpenReference(
      uid, {{"question_id", original.id}, {"reference_id", references.front().as_object().at("id")}}));
  // Starting another training must preserve the daily's repair and assistance.
  const auto other = ordinary.StartSession(uid, {{"recommended", true}});
  Check(other.at("session_id") != session.at("session_id"), "ordinary practice reused the daily session");
  MiniAppService after_restart(bank, pool);
  session = after_restart.StartSession(uid, start);
  Check(session.at("session_id").as_int64() == id && session.at("question").as_object().at("assisted").as_bool() &&
            session.at("progress").as_object().at("attempt").as_uint64() == 2,
        "daily resume lost question, assistance or repair attempt");
  const auto xp_before = repository.LoadProgressSummary(uid).xp_total;
  static_cast<void>(after_restart.SubmitAnswer(
      uid, {{"session_id", id}, {"question_id", original.id}, {"answer", task.correct_option_id}}));
  const auto receipt = after_restart.NextQuestion(uid);
  Check(Text(receipt, "status") == "completed", "corrected daily did not complete");
  Check(after_restart.StartSession(uid, start) == receipt, "daily completion retry created a new session");
  const auto completed = after_restart.DailyPractice(uid, {});
  Check(completed.at("practiced").as_bool() && completed.at("completed").as_bool(), "daily completion was not durable");
  Check(repository.LoadProgressSummary(uid).xp_total == xp_before && repository.LoadCleanQuestionFamilies(uid).empty(),
        "assisted daily completion granted bonus XP or clean evidence");
  const auto saved = repository.LoadSavedTrainingSessions(uid);
  Check(
      saved.size() == 3 &&
          std::ranges::any_of(saved,
                              [&](const auto& item) { return item.session_id == other.at("session_id").as_int64(); }) &&
          std::ranges::any_of(
              saved, [&](const auto& item) { return item.session_id == ordinary_before.at("session_id").as_int64(); }),
      "daily completion discarded a separate unfinished training");

  // Selection remains the existing recommendation, not a new daily-only level policy.
  MiniAppService selection_service(full, pool);
  const auto summary = repository.LoadProgressSummary(uid - 1);
  const auto expected = RecommendTraining(full, summary, {}, {});
  const auto selection_day = selection_service.DailyPractice(uid - 1, {{"timezone", "UTC"}});
  const auto selected =
      selection_service.StartSession(uid - 1, {{"daily", true}, {"daily_day", selection_day.at("local_day")}});
  Check(Text(selected.at("question").as_object(), "id") == expected.question_id,
        "daily bypassed the existing recommendation/prerequisite policy");

  auto advanced_question = bank.Get("block_scope_destruction_order");
  advanced_question.level = 10;  // Synthetic restricted-bank fixture, not a content edit.
  QuestionBank restricted({advanced_question});
  MiniAppService unavailable(restricted, pool);
  const auto no_content = unavailable.DailyPractice(uid - 2, {{"timezone", "UTC"}});
  Check(Text(no_content, "status") == "unavailable", "unsafe content was advertised as ready");
  Reject([&] {
    static_cast<void>(unavailable.StartSession(uid - 2, {{"daily", true}, {"daily_day", no_content.at("local_day")}}));
  });
  Check(!repository.LoadDailyPractice(uid - 2).session_id, "rejected daily created an assignment");
  auto blocked_cards = LoadQuestions("content/questions");
  std::erase_if(blocked_cards, [](const Question& candidate) { return candidate.id != "reference_definition"; });
  QuestionBank prerequisite_blocked(std::move(blocked_cards));
  MiniAppService blocked_service(prerequisite_blocked, pool);
  Check(Text(blocked_service.DailyPractice(uid - 5, {}), "status") == "unavailable",
        "recommendation fallback bypassed unlearned prerequisites");
  const auto removed_day = first.DailyPractice(uid - 3, {});
  static_cast<void>(first.StartSession(uid - 3, {{"daily", true}, {"daily_day", removed_day.at("local_day")}}));
  auto remaining = LoadQuestions("content/questions");
  std::erase_if(remaining, [](const Question& candidate) { return candidate.id == "block_scope_destruction_order"; });
  QuestionBank without_assigned(std::move(remaining));
  MiniAppService after_content_change(without_assigned, pool);
  Check(Text(after_content_change.DailyPractice(uid - 3, {}), "status") == "unavailable" &&
            repository.LoadDailyPractice(uid - 3).session_id.has_value(),
        "missing assigned content was replaced or leaked a snapshot parser error");

  // Controlled attribution fixture: move the assignment to yesterday, not the
  // server clock. Finishing that saved session today must not finish today's task.
  const auto old_day = first.DailyPractice(uid - 4, {});
  const auto yesterday_session = first.StartSession(uid - 4, {{"daily", true}, {"daily_day", old_day.at("local_day")}});
  const auto old_id = yesterday_session.at("session_id").as_int64();
  Sql(url, "BEGIN; UPDATE daily_practice_assignments SET local_day = local_day - 1 WHERE session_id = " +
               std::to_string(old_id) +
               "; UPDATE active_training_sessions a SET state_version = a.state_version + 1, "
               "state_json = jsonb_set(a.state_json, '{daily_day}', to_jsonb(to_char(d.local_day, 'YYYY-MM-DD'))) "
               "FROM daily_practice_assignments d WHERE d.session_id = a.session_id AND a.session_id = " +
               std::to_string(old_id) + "; COMMIT;");
  MiniAppService next_day(bank, pool);
  const auto yesterday = next_day.ResumeSession(uid - 4, {{"session_id", old_id}});
  Check(yesterday.at("daily_day") != old_day.at("local_day"), "calendar fixture did not restore yesterday's marker");
  static_cast<void>(next_day.SubmitAnswer(
      uid - 4, {{"session_id", old_id}, {"question_id", original.id}, {"answer", task.correct_option_id}}));
  static_cast<void>(next_day.NextQuestion(uid - 4));
  const auto today = next_day.DailyPractice(uid - 4, {});
  Check(Text(today, "status") == "ready" && !today.at("practiced").as_bool() && !today.at("completed").as_bool(),
        "yesterday's completion credited today's assignment");
  const auto new_day = next_day.StartSession(uid - 4, {{"daily", true}, {"daily_day", today.at("local_day")}});
  Check(new_day.at("session_id").as_int64() != old_id, "new day reused yesterday's completed assignment");

  const auto concurrent_day = first.DailyPractice(uid - 6, {});
  const boost::json::object resume_daily{{"daily", true}, {"daily_day", concurrent_day.at("local_day")}};
  const auto concurrent_id = first.StartSession(uid - 6, resume_daily).at("session_id").as_int64();
  MiniAppService resume_a(bank, pool), resume_b(bank, pool);
  for (int iteration = 0; iteration < 12; ++iteration) {
    static_cast<void>(ordinary.StartSession(uid - 6, {{"recommended", true}}));
    std::promise<void> signal;
    const auto ready = signal.get_future().share();
    auto resume_one = std::async(std::launch::async, [&] {
      ready.wait();
      return resume_a.StartSession(uid - 6, resume_daily);
    });
    auto resume_two = std::async(std::launch::async, [&] {
      ready.wait();
      return resume_b.StartSession(uid - 6, resume_daily);
    });
    signal.set_value();
    Check(resume_one.get().at("session_id").as_int64() == concurrent_id &&
              resume_two.get().at("session_id").as_int64() == concurrent_id &&
              repository.LoadActiveTrainingSession(uid - 6)->session_id == concurrent_id,
          "concurrent parked daily resume failed or changed assignment");
  }
}
