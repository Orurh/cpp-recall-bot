#include "learning/skill_catalog.h"
#include "miniapp/miniapp_service.h"
#include "storage/attempt_repository.h"
#include "storage/question_loader.h"

#include <libpq-fe.h>
#include <boost/json.hpp>

#include <algorithm>
#include <chrono>
#include <memory>
#include <stdexcept>
#include <string>

namespace {

void Check(bool condition, const char* message) {
  if (!condition) {
    throw std::runtime_error(message);
  }
}

void Sql(PGconn* db, const std::string& sql) {
  std::unique_ptr<PGresult, decltype(&PQclear)> result(PQexec(db, sql.c_str()), PQclear);
  Check(result && PQresultStatus(result.get()) == PGRES_COMMAND_OK, "balanced training SQL fixture failed");
}

std::string Text(const boost::json::object& value, std::string_view key) {
  return std::string(value.at(key).as_string());
}

}  // namespace

void CheckTrainingBalance(const char* url) {
  auto loaded = LoadQuestions("content/questions");
  std::erase_if(loaded,
                [](const Question& question) { return !std::holds_alternative<SingleChoiceTask>(question.task); });
  QuestionBank bank(std::move(loaded));
  auto pool = std::make_shared<DatabaseConnectionPool>(url, 3);
  AttemptRepository repository(pool);
  std::unique_ptr<PGconn, decltype(&PQfinish)> connection(PQconnectdb(url), PQfinish);
  Check(connection && PQstatus(connection.get()) == CONNECTION_OK, "balanced training database unavailable");
  const auto uid = -2'000'000'000 - std::chrono::steady_clock::now().time_since_epoch().count() % 1'000'000'000;
  const auto user = std::to_string(uid);
  repository.EnsureUser(uid);
  Sql(connection.get(), "UPDATE users SET adaptive_level = 5, unlocked_level = 5 WHERE user_id = " + user);
  // Synthetic prior learning: only easy questions become due; advanced material
  // is not made eligible just to fill the new-material quota.
  std::size_t due = 0;
  for (const auto& id : bank.QuestionIds()) {
    const auto& question = bank.Get(id);
    const auto* skill = FindSkill(question.skill_id);
    if (question.level > 2 || skill == nullptr || skill->prerequisites.empty()) {
      continue;
    }
    Sql(connection.get(),
        "INSERT INTO question_progress(user_id, question_id, completed_count, correct_count, "
        "review_stage, next_review_at) VALUES (" +
            user + ", '" + id + "', 1, 1, 1, now() - interval '2 days')");
    ++due;
  }
  Check(due >= 4, "balanced fixture has too few due questions");

  MiniAppService service(bank, pool);
  const TelegramMiniAppUser telegram_user{
      .id = uid, .first_name = {}, .last_name = {}, .username = {}, .language_code = {}};
  const auto bootstrap = service.Bootstrap(telegram_user);
  const auto recommendation = bootstrap.at("recommendation").as_object().at("plan").as_object();
  Check(recommendation.at("new").as_uint64() == 6 && recommendation.at("review").as_uint64() == 4,
        "bootstrap did not describe a balanced primary plan");
  auto session =
      service.StartSession(uid, {{"recommended", true}, {"request_id", "20000000-0000-4000-8000-000000000001"}});
  const auto saved_plan = session.at("training_plan");
  Check(saved_plan.as_object().at("new").as_uint64() == 6 && saved_plan.as_object().at("review").as_uint64() == 4,
        "recommended API still narrowed the whole session to one due question");
  Check(session.at("topics").as_array().empty(), "recommended training was restricted to the due topic");
  const auto first_question = Text(session.at("question").as_object(), "id");
  MiniAppService restored(bank, pool);
  const auto resumed = restored.Bootstrap(telegram_user).at("session").as_object();
  Check(Text(resumed.at("question").as_object(), "id") == first_question && resumed.at("training_plan") == saved_plan,
        "restart changed the current card or balanced plan");
  const auto retry =
      restored.StartSession(uid, {{"recommended", true}, {"request_id", "20000000-0000-4000-8000-000000000001"}});
  Check(Text(retry.at("question").as_object(), "id") == first_question && retry.at("training_plan") == saved_plan,
        "an idempotent start rebuilt the saved plan");

  int easy_checked = 0;
  for (int index = 0; index < 10; ++index) {
    const auto& question = bank.Get(Text(session.at("question").as_object(), "id"));
    const auto before = repository.GetAdaptiveState(uid);
    const auto& answer = std::get<SingleChoiceTask>(question.task).correct_option_id;
    const boost::json::object request{{"question_id", question.id}, {"answer", answer}};
    const auto receipt = restored.SubmitAnswer(uid, request);
    if (index == 0) {
      Check(restored.SubmitAnswer(uid, request) == receipt, "a lost answer receipt changed the balanced session");
    }
    const auto after = repository.GetAdaptiveState(uid);
    if (question.level < before.level) {
      ++easy_checked;
      Check(before.level == after.level && before.evidence == after.evidence,
            "an easy card in a mixed session raised the overall adaptive evidence");
    }
    session = restored.NextQuestion(uid);
    if (index < 9) {
      Check(Text(session, "status") == "active", "mixed training finished before all planned cards");
    }
  }
  Check(Text(session, "status") == "completed" && easy_checked >= 4,
        "the balanced plan did not complete or omitted easy review coverage");
  Check(repository.LoadReviewStates(uid).size() >= due + 6,
        "new answers were not stored independently from previously learned cards");

  const auto manual = restored.StartSession(
      uid, {{"level", 1}, {"topics", boost::json::array{}}, {"modes", boost::json::array{"theory", "api", "code"}}});
  Check(!manual.contains("training_plan") && manual.at("selected_level").as_int64() == 1 &&
            !manual.at("tracks_level_progress").as_bool(),
        "manual replay was converted to mixed or progression practice");
  static_cast<void>(restored.StopSession(uid));
}
