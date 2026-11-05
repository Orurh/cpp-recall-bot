#include "miniapp/miniapp_service.h"
#include "miniapp/session_snapshot.h"
#include "quiz/quiz_service.h"
#include "storage/attempt_repository.h"
#include "storage/question_loader.h"

#include <libpq-fe.h>

#include <chrono>
#include <memory>
#include <stdexcept>

namespace {
void Check(bool value, const char* message) {
  if (!value)
    throw std::runtime_error(message);
}
std::string Text(const boost::json::object& value, std::string_view key) {
  return std::string(value.at(key).as_string());
}
template <typename F>
void Reject(F&& operation) {
  bool rejected = false;
  try {
    operation();
  } catch (const std::exception&) {
    rejected = true;
  }
  Check(rejected, "invalid generated exercise request/snapshot accepted");
}
}  // namespace

void CheckGeneratedVector(const char* url) {
  auto pool = std::make_shared<DatabaseConnectionPool>(url, 3);
  AttemptRepository repository(pool);
  QuestionBank bank(LoadQuestions("content/questions"));
  std::unique_ptr<PGconn, decltype(&PQfinish)> db(PQconnectdb(url), PQfinish);
  Check(db && PQstatus(db.get()) == CONNECTION_OK, "generated vector test DB unavailable");
  const auto uid = -24'000'000'000 - std::chrono::steady_clock::now().time_since_epoch().count() % 1'000'000'000;
  repository.EnsureUser(uid);
  const auto sql = [&](const std::string& text) {
    std::unique_ptr<PGresult, decltype(&PQclear)> result(PQexec(db.get(), text.c_str()), PQclear);
    Check(result && PQresultStatus(result.get()) == PGRES_COMMAND_OK, "generated vector fixture SQL failed");
  };
  MiniAppService service(bank, pool);
  const TelegramMiniAppUser user{.id = uid, .first_name = {}, .last_name = {}, .username = {}, .language_code = {}};
  Check(!service.Bootstrap(user).at("vector_generator").as_object().at("available").as_bool(),
        "new generator bypassed prerequisites");
  Reject([&] { static_cast<void>(service.StartSession(uid, {{"exercise_generator", kVectorSizeGenerator}})); });
  sql("INSERT INTO skill_progress(user_id,skill_id,question_id) VALUES (" + std::to_string(uid) +
      ",'core.storage_duration','gen01-prerequisite')");
  sql("INSERT INTO user_skill_state(user_id,skill_id,status) VALUES (" + std::to_string(uid) +
      ",'core.storage_duration','demonstrated')");
  Check(service.Bootstrap(user).at("vector_generator").as_object().at("available").as_bool(),
        "generator unavailable despite prerequisites");
  const boost::json::object start{{"exercise_generator", kVectorSizeGenerator},
                                  {"request_id", "51000000-0000-4000-8000-000000000001"}};
  const auto first = service.StartSession(uid, start);
  const auto sid = first.at("session_id");
  const auto question = first.at("question").as_object();
  Check(question.at("task").as_object().at("type") == "short_answer" && !question.contains("explanation") &&
            !question.at("task").as_object().contains("accepted_answers") && !question.contains("generator") &&
            !question.contains("seed"),
        "generated answer/descriptor leaked");
  Check(first.at("progress").as_object().at("session_goal").as_uint64() == 1 &&
            !first.at("tracks_level_progress").as_bool(),
        "generated practice altered level progression");
  const auto saved = repository.LoadActiveTrainingSession(uid);
  Check(saved.has_value(), "generated exercise not saved");
  const auto snapshot = ParseMiniAppSessionSnapshot(saved->state_json, saved->session_id, saved->state_version, bank);
  const auto expected = snapshot.quiz.generated_vector->question;
  const auto answer = std::get<ShortAnswerTask>(expected.task).accepted_answers.front();
  Check(CheckAnswer(expected, " 00" + answer + " ") == AnswerResult::kCorrect &&
            CheckAnswer(expected, answer + ".0") == AnswerResult::kIncorrect,
        "numeric checking is not decimal-only");
  MiniAppService restarted(bank, pool);
  Check(restarted.CurrentSession(uid).at("question") == first.at("question") &&
            restarted.StartSession(uid, start).at("question") == first.at("question"),
        "restore/retry changed generated code");
  for (const auto& invalid : {boost::json::object{{"exercise_generator", kVectorSizeGenerator}, {"seed", 5}},
                              boost::json::object{{"exercise_generator", kVectorSizeGenerator}, {"balanced", true}}})
    Reject([&] { static_cast<void>(restarted.StartSession(uid, invalid)); });
  auto damaged = boost::json::parse(saved->state_json).as_object();
  damaged.at("quiz").as_object().at("generated_vector").as_object()["seed"] = "-1";
  Reject([&] {
    static_cast<void>(ParseMiniAppSessionSnapshot(boost::json::serialize(damaged), saved->session_id, 1, bank));
  });
  damaged = boost::json::parse(saved->state_json).as_object();
  damaged.at("quiz").as_object().at("generated_vector").as_object().at("content").as_object()["code"] = "changed";
  Reject([&] {
    static_cast<void>(ParseMiniAppSessionSnapshot(boost::json::serialize(damaged), saved->session_id, 1, bank));
  });
  const auto submit = [&](MiniAppService& target, const boost::json::value& session_id, const std::string& value) {
    return target.SubmitAnswer(
        uid, {{"session_id", session_id}, {"question_id", kVectorSizeBaseQuestion}, {"answer", value}});
  };
  const auto receipt = submit(restarted, sid, answer);
  const auto feedback = receipt.at("session").as_object().at("feedback").as_object();
  Check(feedback.at("correct").as_bool() && Text(feedback, "explanation") == expected.explanation &&
            feedback.at("skill_mastery").as_object().at("clean_families").as_int64() == 1 &&
            feedback.at("skill_mastery").as_object().at("current") == "applying",
        "generated trace/evidence incorrect");
  Check(submit(restarted, sid, answer) == receipt, "duplicate generated answer awarded twice");
  const auto xp = repository.LoadProgressSummary(uid).xp_total;
  const auto second = restarted.StartSession(
      uid, {{"exercise_generator", kVectorSizeGenerator}, {"request_id", "51000000-0000-4000-8000-000000000002"}});
  Check(second.at("question").as_object().at("code") != question.at("code"),
        "new session repeated exact generated program");
  // Starting another exercise must preserve the earlier unfinished training/feedback.
  const auto resumed = restarted.ResumeSession(uid, {{"session_id", sid}});
  Check(resumed.at("feedback") == receipt.at("session").as_object().at("feedback"), "old generated feedback lost");
  static_cast<void>(restarted.ResumeSession(uid, {{"session_id", second.at("session_id")}}));
  const auto next_saved = repository.LoadActiveTrainingSession(uid);
  const auto next =
      ParseMiniAppSessionSnapshot(next_saved->state_json, next_saved->session_id, next_saved->state_version, bank);
  const auto next_question = next.quiz.generated_vector->question;
  const auto next_answer = std::get<ShortAnswerTask>(next_question.task).accepted_answers.front();
  const auto wrong = submit(restarted, second.at("session_id"), "999").at("session").as_object();
  Check(!wrong.at("feedback").as_object().at("correct").as_bool() &&
            !wrong.at("feedback").as_object().contains("explanation"),
        "wrong answer bypassed ordinary repair/help rules");
  MiniAppService repaired(bank, pool);
  static_cast<void>(repaired.CurrentSession(uid));
  const auto help = repaired.OpenTheory(uid, {{"session_id", second.at("session_id")},
                                              {"question_id", kVectorSizeBaseQuestion},
                                              {"content_revision", next_question.content_revision},
                                              {"kind", "answer"}});
  Check(boost::json::serialize(help).find("reserve") != std::string::npos,
        "generated archived explanation unavailable");
  static_cast<void>(repaired.NextQuestion(uid));
  const auto final =
      submit(repaired, second.at("session_id"), next_answer).at("session").as_object().at("feedback").as_object();
  Check(final.at("correct").as_bool() && final.at("assisted").as_bool() && final.at("xp_gained").as_int64() == 0 &&
            final.at("skill_mastery").as_object().at("successful_families").as_int64() == 1 &&
            final.at("skill_mastery").as_object().at("clean_families").as_int64() == 1 &&
            repository.LoadProgressSummary(uid).xp_total == xp,
        "variant/help inflated XP or family count");
  // A familiar base question remains repeatable when an earlier prerequisite needs recovery.
  sql("UPDATE user_skill_state SET status='needs_recovery' WHERE user_id=" + std::to_string(uid) +
      " AND skill_id='core.storage_duration'");
  Check(repaired.Bootstrap(user).at("vector_generator").as_object().at("available").as_bool(),
        "familiar generated review was blocked as new material");
  const auto third = repaired.StartSession(uid, {{"exercise_generator", kVectorSizeGenerator}});
  const auto third_saved = repository.LoadActiveTrainingSession(uid);
  const auto third_snapshot =
      ParseMiniAppSessionSnapshot(third_saved->state_json, third_saved->session_id, third_saved->state_version, bank);
  const auto third_answer =
      std::get<ShortAnswerTask>(third_snapshot.quiz.generated_vector->question.task).accepted_answers.front();
  const auto third_feedback =
      submit(repaired, third.at("session_id"), third_answer).at("session").as_object().at("feedback").as_object();
  const auto third_mastery = third_feedback.at("skill_mastery").as_object();
  Check(third_feedback.at("correct").as_bool() && !third_feedback.at("assisted").as_bool() &&
            third_feedback.at("xp_gained").as_int64() == 0 && third_mastery.at("clean_families").as_int64() == 1 &&
            third_mastery.at("application_families").as_int64() == 1 && third_mastery.at("current") == "applying",
        "a new unassisted variant manufactured a reward/independent mastery evidence");
}
