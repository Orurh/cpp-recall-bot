#include "miniapp/miniapp_service.h"
#include "quiz/question_identity.h"
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
  if (!condition)
    throw std::runtime_error(message);
}

std::string Sql(PGconn* db, const std::string& query) {
  std::unique_ptr<PGresult, decltype(&PQclear)> result(PQexec(db, query.c_str()), PQclear);
  if (!result || (PQresultStatus(result.get()) != PGRES_TUPLES_OK && PQresultStatus(result.get()) != PGRES_COMMAND_OK))
    throw std::runtime_error("delayed practice SQL failed: " + std::string(PQerrorMessage(db)));
  return PQntuples(result.get()) ? PQgetvalue(result.get(), 0, 0) : "";
}

template <typename Operation>
void Reject(Operation&& operation) {
  bool rejected = false;
  try {
    operation();
  } catch (const std::invalid_argument&) {
    rejected = true;
  }
  Check(rejected, "delayed practice accepted an unavailable or forged request");
}
}  // namespace

void CheckDelayedPractice(const char* url) {
  const auto cards = LoadQuestions("content/questions");
  QuestionBank bank(cards);
  auto pool = std::make_shared<DatabaseConnectionPool>(url, 3);
  AttemptRepository repository(pool);
  MiniAppService service(bank, pool);
  std::unique_ptr<PGconn, decltype(&PQfinish)> db(PQconnectdb(url), PQfinish);
  Check(db && PQstatus(db.get()) == CONNECTION_OK, "delayed practice database unavailable");
  const auto uid = 21'000'000'000 + std::chrono::steady_clock::now().time_since_epoch().count() % 1'000'000'000;
  const TelegramMiniAppUser user{
      .id = uid, .first_name = "Synthetic", .last_name = {}, .username = {}, .language_code = {}};
  const auto empty = service.Bootstrap(user);
  Check(empty.contains("delayed_practice"), "Bootstrap lacks delayed-practice availability");
  Check(empty.at("delayed_practice").is_null(), "a beginner received an unprepared delayed control");
  Reject([&] { service.StartSession(uid, {{"delayed_practice", "raii_state_restore/v1"}}); });

  // All dates and mastery below are synthetic, on the disposable test database.
  const auto tracking = Sql(db.get(), "SELECT extract(epoch FROM started_at)::TEXT FROM study_contact_tracking");
  const auto analytics = Sql(db.get(), "SELECT extract(epoch FROM enabled_at)::TEXT FROM analytics_meta");
  struct RestoreTracking {
    PGconn* db;
    std::string tracking, analytics;
    ~RestoreTracking() {
      PQclear(PQexec(db, ("UPDATE study_contact_tracking SET started_at=to_timestamp(" + tracking +
                          "); UPDATE analytics_meta SET enabled_at=to_timestamp(" + analytics + ");")
                             .c_str()));
    }
  } restore{db.get(), tracking, analytics};
  Sql(db.get(),
      "UPDATE study_contact_tracking SET started_at=now()-INTERVAL '40 days'; "
      "UPDATE analytics_meta SET enabled_at=now()-INTERVAL '40 days'");
  const auto owner = std::to_string(uid);
  const auto seed = [&](const Question& card) {
    const auto sid = repository.StartSession(uid, "miniapp:synthetic-delayed-preparation");
    static_cast<void>(repository.RecordSubmission({.user_id = uid,
                                                   .session_id = sid,
                                                   .question_id = card.id,
                                                   .topic = card.topic,
                                                   .skill_id = card.skill_id,
                                                   .question_family_id = card.question_family_id,
                                                   .evidence_type = card.evidence_type,
                                                   .transfer = card.transfer,
                                                   .answer = "synthetic",
                                                   .correct = true,
                                                   .completed = true,
                                                   .attempt_number = 1,
                                                   .content_revision = card.content_revision}));
    repository.FinishSession(sid, "completed");
  };
  for (const auto skill : {"references.fundamentals", "oop.objects", "core.storage_duration"}) {
    const auto card = std::ranges::find(cards, skill, &Question::skill_id);
    Check(card != cards.end(), "pilot prerequisite fixture missing");
    seed(*card);
  }
  seed(bank.Get("vector_raii_early_return"));
  Sql(db.get(), "UPDATE user_skill_state SET status='demonstrated' WHERE user_id=" + owner);
  const auto age = [&](int hours) {
    const auto interval = "now()-INTERVAL '" + std::to_string(hours) + " hours'";
    Sql(db.get(), "UPDATE attempts SET answered_at=" + interval + " WHERE user_id=" + owner +
                      "; UPDATE learning_events SET occurred_at=" + interval + " WHERE user_id=" + owner);
  };
  age(71);
  Check(service.Bootstrap(user).at("delayed_practice").is_null(), "control was offered before its pause");
  age(80);
  if (!repository.LoadDelayedPracticeState(uid).ready)
    throw std::runtime_error(
        "tracked preparation not eligible: " +
        Sql(db.get(),
            "SELECT json_agg(json_build_object('card',a.question_id,'skill',r.content_snapshot->>'skill_id',"
            "'revision',a.content_revision,'correct',a.correct,'help',a.assisted,'attempt',a.attempt_number,"
            "'known',(SELECT count(*) FROM learning_events e WHERE e.event_key='answer:'||a.id),"
            "'hours',extract(epoch FROM now()-a.answered_at)/3600))::TEXT "
            "FROM attempts a LEFT JOIN question_content_revisions r ON r.question_id=a.question_id "
            "AND r.content_revision=a.content_revision WHERE a.user_id=" +
                owner));
  Check(!service.Bootstrap(user).at("delayed_practice").is_null(),
        "pinned bank or topic prerequisites rejected preparation");
  const auto ready = service.Bootstrap(user).at("delayed_practice").as_object();
  Check(ready.at("status") == "ready" && !ready.at("familiar").as_bool(), "prepared control was not offered");
  Sql(db.get(), "UPDATE study_contact_tracking SET started_at=now()");
  Check(!repository.LoadDelayedPracticeState(uid).ready, "pre-tracking history counted as known preparation");
  Sql(db.get(), "UPDATE study_contact_tracking SET started_at=now()-INTERVAL '40 days'");
  const auto& prerequisite = *std::ranges::find(cards, "references.fundamentals", &Question::skill_id);
  Sql(db.get(),
      "UPDATE attempts SET assisted=true WHERE user_id=" + owner + " AND question_id='" + prerequisite.id + "'");
  Check(!repository.LoadDelayedPracticeState(uid).ready, "assisted prerequisite counted as independent knowledge");
  Sql(db.get(), "UPDATE attempts SET assisted=false, content_revision='' WHERE user_id=" + owner +
                    " AND question_id='" + prerequisite.id + "'");
  Check(!repository.LoadDelayedPracticeState(uid).ready, "unknown prerequisite revision was accepted");
  Sql(db.get(), "UPDATE attempts SET content_revision='" + prerequisite.content_revision + "' WHERE user_id=" + owner +
                    " AND question_id='" + prerequisite.id + "'");
  Check(repository.LoadDelayedPracticeState(uid).ready, "restored clean prerequisite did not recover availability");
  auto revised_cards = cards;
  auto& revised_preparation = *std::ranges::find(revised_cards, "vector_raii_early_return", &Question::id);
  revised_preparation.content_revision = std::string(64, '0');
  QuestionBank revised_bank(std::move(revised_cards));
  MiniAppService revised_service(revised_bank, pool);
  Check(revised_service.Bootstrap(user).at("delayed_practice").is_null(), "a revised pilot retained its old offer");
  Reject([&] { revised_service.StartSession(uid, {{"delayed_practice", "raii_state_restore/v1"}}); });
  Sql(db.get(), "UPDATE learning_events SET content_revision='' WHERE user_id=" + owner +
                    " AND question_id='vector_raii_early_return' AND event_name='answer'");
  Check(!repository.LoadDelayedPracticeState(uid).ready, "unknown preparation provenance was treated as recorded");
  Sql(db.get(),
      "UPDATE learning_events e SET content_revision=a.content_revision FROM attempts a "
      "WHERE e.event_key='answer:'||a.id AND e.user_id=" +
          owner);
  Check(repository.LoadDelayedPracticeState(uid).ready, "restored known preparation did not recover availability");
  Sql(db.get(), "UPDATE attempts SET answered_at=now()+INTERVAL '1 minute' WHERE user_id=" + owner +
                    " AND question_id='vector_raii_early_return'");
  Check(!repository.LoadDelayedPracticeState(uid).ready, "future activity bypassed the pause");
  age(80);
  Check(Sql(db.get(), "SELECT count(*) FROM study_contacts WHERE user_id=" + owner) == "0",
        "an availability preview disclosed or journalled the control");
  Reject([&] { service.StartSession(uid, {{"delayed_practice", "unknown"}}); });
  Reject([&] { service.StartSession(uid, {{"delayed_practice", "raii_state_restore/v1"}, {"level", 10}}); });
  const auto ordinary = service.StartSession(uid, {{"recommended", true}});
  // Starting ordinary practice may itself contact RAII; it must postpone the offer.
  const auto ordinary_sid = ordinary.at("session_id").as_int64();
  Sql(db.get(), "UPDATE study_contacts SET offered_at=now()-INTERVAL '80 hours' WHERE user_id=" + owner);
  const boost::json::object start{{"delayed_practice", "raii_state_restore/v1"},
                                  {"request_id", "23000000-0000-4000-8000-000000000001"}};
  const auto control = service.StartSession(uid, start);
  const auto sid = control.at("session_id").as_int64();
  Check(control.at("question").as_object().at("id") == "raii_state_rollback_commit",
        "delayed practice did not select its pinned control");
  Check(control.at("delayed_practice_id") == "raii_state_restore/v1", "control marker missing");
  Check(service.StartSession(uid, start).at("session_id").as_int64() == sid, "retry duplicated the control");
  Check(!repository.LoadSavedTrainingSessions(uid).empty(), "starting control discarded the ordinary training");
  MiniAppService restarted(bank, pool);
  Check(restarted.Bootstrap(user).at("delayed_practice").as_object().at("status") == "in_progress",
        "restart lost the unfinished control");
  const auto question = control.at("question").as_object();
  const auto references = question.at("references").as_array();
  Check(!references.empty(), "pilot control must retain optional help");
  restarted.OpenReference(uid, {{"session_id", sid},
                                {"question_id", "raii_state_rollback_commit"},
                                {"reference_id", references.front().as_object().at("id")}});
  static_cast<void>(restarted.ResumeSession(uid, {{"session_id", ordinary_sid}}));
  // The original response may be lost, then another device parks this control.
  // Retrying that same UUID must recover it, not report it as completed.
  const auto resumed = restarted.StartSession(uid, start);
  Check(resumed.at("session_id").as_int64() == sid, "saved control was replaced by a new session");
  Check(resumed.at("question").as_object().at("assisted").as_bool(), "control help was lost on resume");
  const auto saved = repository.LoadSavedTrainingSessions(uid);
  Check(std::ranges::find(saved, ordinary_sid, &ActiveTrainingSessionRecord::session_id) != saved.end(),
        "UUID retry discarded the other device's ordinary training");
  const auto answer = std::get<SingleChoiceTask>(bank.Get("raii_state_rollback_commit").task).correct_option_id;
  const auto receipt = restarted.SubmitAnswer(
      uid, {{"session_id", sid}, {"question_id", "raii_state_rollback_commit"}, {"answer", answer}});
  const auto& feedback = receipt.at("session").as_object().at("feedback").as_object();
  Check(feedback.at("correct").as_bool() && feedback.at("xp_gained").as_int64() == 0,
        "helped delayed practice bypassed ordinary assistance rules");
  const auto completion = restarted.NextQuestion(uid, {{"session_id", sid}});
  Check(completion.at("status") == "completed", "one-card delayed practice did not finish");
  Check(restarted.Bootstrap(user).at("delayed_practice").is_null(), "help/early exposure did not postpone practice");
  Sql(db.get(), "UPDATE study_contacts SET offered_at=now()-INTERVAL '337 hours' WHERE user_id=" + owner);
  age(335);
  Check(!repository.LoadDelayedPracticeState(uid).ready, "familiar control used the first-exposure window");
  age(337);
  const auto familiar = restarted.Bootstrap(user).at("delayed_practice").as_object();
  Check(familiar.at("familiar").as_bool(), "previously exposed control was presented as new");
  const TelegramMiniAppUser other{
      .id = uid + 1, .first_name = {}, .last_name = {}, .username = {}, .language_code = {}};
  Check(restarted.Bootstrap(other).at("delayed_practice").is_null(), "another user's preparation leaked");
  const auto last_contact = Sql(db.get(), "SELECT max(id) FROM study_contacts WHERE user_id=" + owner);
  Sql(db.get(),
      "INSERT INTO "
      "study_contacts(user_id,session_id,question_id,question_content_revision,attempt_number,kind,offered_at) "
      "SELECT user_id,session_id,question_id,question_content_revision,attempt_number,'question_delivered',offered_at "
      "FROM (SELECT * FROM study_contacts WHERE user_id=" +
          owner + " LIMIT 1) c CROSS JOIN generate_series(1,50001)");
  Check(!repository.LoadDelayedPracticeState(uid).ready, "truncated history was accepted as complete");
  Sql(db.get(), "DELETE FROM study_contacts WHERE user_id=" + owner + " AND id>" + last_contact);
}
