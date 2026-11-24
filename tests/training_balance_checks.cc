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
#include <unordered_set>
#include <vector>

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
      Check(before == after, "an easy card in a mixed session raised the overall adaptive evidence");
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
  const auto before_manual = repository.GetAdaptiveState(uid);
  const auto& manual_question = bank.Get(Text(manual.at("question").as_object(), "id"));
  static_cast<void>(
      restored.SubmitAnswer(uid, {{"question_id", manual_question.id},
                                  {"answer", std::get<SingleChoiceTask>(manual_question.task).correct_option_id}}));
  Check(repository.GetAdaptiveState(uid) == before_manual, "lower manual practice changed overall family evidence");
  static_cast<void>(restored.StopSession(uid));

  // Real card metadata, a synthetic account, and exactly three independent
  // families make persistence between HTTP service instances observable.
  std::vector<Question> progression_cards;
  std::unordered_set<std::string> progression_families;
  for (const auto& id : bank.QuestionIds()) {
    const auto& card = bank.Get(id);
    if (card.topic == "containers" && card.level == 3 && card.evidence_type == QuestionEvidenceType::kApplication &&
        progression_families.insert(card.question_family_id).second) {
      progression_cards.push_back(card);
      if (progression_cards.size() == 3)
        break;
    }
  }
  Check(progression_cards.size() == 3, "progression fixture needs three actual L3 application families");
  // Three independent questions suffice for manual selection; unrelated
  // topics must not be required to reach the availability threshold.
  QuestionBank progression_bank(std::move(progression_cards));
  const auto progression_user = uid - 1;
  MiniAppService starter(progression_bank, pool);
  auto progression_session = starter.StartSession(
      progression_user,
      {{"level", 3}, {"topics", boost::json::array{"containers"}}, {"modes", boost::json::array{"code"}}});
  for (int answered = 0; answered < 3; ++answered) {
    MiniAppService fresh_worker(progression_bank, pool);
    const auto resumed_progression = fresh_worker.CurrentSession(progression_user);
    Check(resumed_progression.at("question") == progression_session.at("question"),
          "service restart changed the pending progression card");
    const auto& card = progression_bank.Get(Text(resumed_progression.at("question").as_object(), "id"));
    const boost::json::object request{{"question_id", card.id},
                                      {"answer", std::get<SingleChoiceTask>(card.task).correct_option_id}};
    const auto receipt = fresh_worker.SubmitAnswer(progression_user, request);
    const auto global = repository.GetAdaptiveState(progression_user);
    const auto topic = repository.LoadTopicStates(progression_user).at("containers");
    Check(global == topic && global.level == (answered < 2 ? 3 : 4) &&
              global.promotion.families.size() == (answered < 2 ? static_cast<std::size_t>(answered + 1) : 0),
          "service restart lost family evidence or promotion bypassed three distinct clean families");
    Check(fresh_worker.SubmitAnswer(progression_user, request) == receipt &&
              repository.GetAdaptiveState(progression_user) == global,
          "replayed API answer changed progression evidence");
    progression_session = fresh_worker.NextQuestion(progression_user);
  }
  Check(Text(progression_session, "status") == "completed" && repository.GetUnlockedLevel(progression_user) == 4,
        "three independent clean applications did not unlock the next difficulty");

  // Regression: sparse L8-L10 must be available both in the home metadata and
  // through the manual session API. Two cards are still insufficient.
  const auto all_expert_cards = LoadQuestions("content/questions");
  QuestionBank full_bank(all_expert_cards);
  MiniAppService full_service(full_bank, pool);
  for (int level = 8; level <= 10; ++level) {
    std::vector<Question> cards;
    std::unordered_set<std::string> families;
    for (const auto& question : all_expert_cards) {
      if (question.topic != "containers" || question.level != level ||
          !families.insert(question.question_family_id).second)
        continue;
      cards.push_back(question);
      if (cards.size() == 3)
        break;
    }
    Check(cards.size() == 3, "expert fixture requires three families");

    QuestionBank sparse_bank(cards);
    const auto learner = uid - 100 - level;
    repository.EnsureUser(learner);
    Sql(connection.get(), "UPDATE users SET unlocked_level=10 WHERE user_id=" + std::to_string(learner));
    MiniAppService sparse_service(sparse_bank, pool);
    const TelegramMiniAppUser visitor{
        .id = learner, .first_name = {}, .last_name = {}, .username = {}, .language_code = {}};
    const auto home = full_service.Bootstrap(visitor);
    bool found = false;
    for (const auto& item : home.at("levels").as_array()) {
      const auto& entry = item.as_object();
      if (entry.at("level").as_int64() != level)
        continue;
      found = true;
      Check(entry.at("question_count").as_uint64() == 3 && entry.at("ready").as_bool() &&
                entry.at("unlocked").as_bool(),
            "three-card expert level is locked on the home screen");
    }
    Check(found, "expert level is absent from home metadata");

    const auto expert_session = sparse_service.StartSession(
        learner, {{"level", level}, {"topics", boost::json::array{"containers"}},
                  {"modes", boost::json::array{"theory", "api", "code"}}});
    Check(expert_session.at("status") == "active" && expert_session.at("selected_level").as_int64() == level &&
              expert_session.at("progress").as_object().at("session_goal").as_uint64() == 3,
          "three-card expert level could not start a short session");

    if (level != 8)
      continue;
    std::vector<Question> underfilled_questions;
    int retained = 0;
    for (const auto& question : all_expert_cards) {
      if (question.topic == "containers" && question.level == level && retained++ >= 2)
        continue;
      underfilled_questions.push_back(question);
    }
    QuestionBank insufficient_bank(std::move(underfilled_questions));
    const auto underfilled = learner - 200;
    repository.EnsureUser(underfilled);
    Sql(connection.get(), "UPDATE users SET unlocked_level=10 WHERE user_id=" + std::to_string(underfilled));
    MiniAppService insufficient_service(insufficient_bank, pool);
    const TelegramMiniAppUser underfilled_visitor{
        .id = underfilled, .first_name = {}, .last_name = {}, .username = {}, .language_code = {}};
    const auto underfilled_home = insufficient_service.Bootstrap(underfilled_visitor);
    bool underfilled_found = false;
    for (const auto& item : underfilled_home.at("levels").as_array()) {
      if (item.as_object().at("level").as_int64() != level)
        continue;
      underfilled_found = true;
      Check(!item.as_object().at("ready").as_bool() && !item.as_object().at("unlocked").as_bool(),
            "two-card expert level is advertised as ready");
    }
    Check(underfilled_found, "underfilled level is absent from home metadata");
    bool rejected = false;
    try {
      static_cast<void>(insufficient_service.StartSession(
          underfilled, {{"level", level}, {"topics", boost::json::array{"containers"}},
                        {"modes", boost::json::array{"theory", "api", "code"}}}));
    } catch (const std::invalid_argument&) {
      rejected = true;
    }
    Check(rejected, "two-card expert level bypassed the coverage gate");
  }
}
