#include "learning/reference_catalog.h"
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
#include <string_view>
#include <utility>
#include <vector>

namespace {
void Check(bool condition, const char* message) {
  if (!condition)
    throw std::runtime_error(message);
}

std::string Sql(PGconn* db, const std::string& query) {
  std::unique_ptr<PGresult, decltype(&PQclear)> result(PQexec(db, query.c_str()), PQclear);
  if (!result || (PQresultStatus(result.get()) != PGRES_TUPLES_OK && PQresultStatus(result.get()) != PGRES_COMMAND_OK))
    throw std::runtime_error("study contact SQL failed: " + std::string(PQerrorMessage(db)));
  return PQntuples(result.get()) == 0 ? "" : PQgetvalue(result.get(), 0, 0);
}

template <typename Operation>
void Reject(Operation&& operation) {
  bool rejected = false;
  try {
    operation();
  } catch (const std::exception&) {
    rejected = true;
  }
  Check(rejected, "invalid or unjournalled content was returned");
}

struct FaultCleanup {
  PGconn* db;
  ~FaultCleanup() {
    PQclear(PQexec(db,
                   "DROP TRIGGER IF EXISTS mem03b_fault ON study_contacts; "
                   "DROP FUNCTION IF EXISTS mem03b_fault();"));
  }
};
}  // namespace

void CheckStudyContacts(const char* url) {
  const auto cards = LoadQuestions("content/questions");
  const auto target = std::ranges::find(cards, "c_str_from_temporary_dangles", &Question::id);
  Check(target != cards.end(), "study contact fixture missing");
  std::vector<Question> fixture{*target};
  for (const auto& card : cards) {
    if (fixture.size() == 5)
      break;
    if (card.level == target->level && card.topic != target->topic)
      fixture.push_back(card);
  }
  QuestionBank bank(std::move(fixture));
  auto pool = std::make_shared<DatabaseConnectionPool>(url, 3);
  AttemptRepository repository(pool);
  MiniAppService service(bank, pool);
  std::unique_ptr<PGconn, decltype(&PQfinish)> db(PQconnectdb(url), PQfinish);
  Check(db && PQstatus(db.get()) == CONNECTION_OK, "study contact database unavailable");
  const auto uid = 14'000'000'000 + std::chrono::steady_clock::now().time_since_epoch().count() % 1'000'000'000;
  const auto initial = service.StartSession(
      uid,
      {{"modes", boost::json::array{"code"}}, {"topics", boost::json::array{target->topic}}, {"level", target->level}});
  const auto sid = initial.at("session_id").as_int64();
  const auto filter = "user_id=" + std::to_string(uid);
  const auto count = [&](std::string_view kind) {
    return std::stoll(
        Sql(db.get(), "SELECT count(*) FROM study_contacts WHERE " + filter + " AND kind='" + std::string(kind) + "'"));
  };
  Check(count("question_delivered") == 1, "card offered without an answer was not journalled");
  const auto saved = repository.LoadActiveTrainingSession(uid);
  static_cast<void>(service.CurrentSession(uid));
  MiniAppService restarted(bank, pool);
  static_cast<void>(restarted.CurrentSession(uid));
  Check(count("question_delivered") == 3, "repeat delivery or restart lost a contact");
  Check(repository.LoadActiveTrainingSession(uid)->state_version == saved->state_version,
        "contact tracking changed the learning snapshot");
  const auto entries = ReferencesForQuestion(*target);
  Check(!entries.empty(), "study contact fixture has no reference");
  const std::string reference_id(entries.front()->id);
  const boost::json::object help{{"question_id", target->id}, {"reference_id", reference_id}, {"session_id", sid}};
  // A failed journal write cannot return help. Its already committed assistance
  // marker must survive retry/restart and cannot become an unassisted answer.
  FaultCleanup cleanup{db.get()};
  Sql(db.get(),
      "CREATE FUNCTION mem03b_fault() RETURNS trigger LANGUAGE plpgsql AS $$ BEGIN "
      "RAISE EXCEPTION 'controlled journal failure'; END; $$; "
      "CREATE TRIGGER mem03b_fault BEFORE INSERT ON study_contacts FOR EACH ROW "
      "WHEN (NEW.user_id=" +
          std::to_string(uid) +
          " AND NEW.kind='reference_opened') "
          "EXECUTE FUNCTION mem03b_fault();");
  Reject([&] { static_cast<void>(restarted.OpenReference(uid, help)); });
  Check(count("reference_opened") == 0, "failed journal write persisted a contact");
  const auto assisted = boost::json::parse(repository.LoadActiveTrainingSession(uid)->state_json).as_object();
  Check(assisted.at("reference_viewed_question_ids").as_array().size() == 1,
        "failed journal acknowledgement lost the durable help marker");
  Sql(db.get(), "DROP TRIGGER mem03b_fault ON study_contacts; DROP FUNCTION mem03b_fault();");
  MiniAppService after_fault_restart(bank, pool);
  const auto first = after_fault_restart.OpenReference(uid, help);
  const auto views = repository.LoadReferenceViews(uid, sid, target->id, target->content_revision);
  const auto help_state = repository.LoadActiveTrainingSession(uid);
  Check(restarted.OpenReference(uid, help) == first && count("reference_opened") == 2,
        "repeat reference opening was lost or changed its archived content");
  const auto repeated_views = repository.LoadReferenceViews(uid, sid, target->id, target->content_revision);
  Check(views.size() == 1 && repeated_views.size() == 1 &&
            repeated_views.front().opened_at == views.front().opened_at &&
            repository.LoadActiveTrainingSession(uid)->state_version == help_state->state_version,
        "repeat opening rewrote first-opening provenance or learning state");
  const auto& task = std::get<SingleChoiceTask>(target->task);
  const auto wrong =
      std::ranges::find_if(task.options, [&](const auto& option) { return option.id != task.correct_option_id; });
  static_cast<void>(restarted.SubmitAnswer(uid, {{"question_id", target->id}, {"answer", wrong->id}}));
  const auto before_help = repository.LoadProgressSummary(uid);
  Check(count("feedback_delivered") == 1, "wrong-answer feedback was not journalled");
  const boost::json::object principle{{"session_id", sid}, {"question_id", target->id}, {"kind", "principle"}};
  static_cast<void>(restarted.OpenTheory(uid, principle));
  static_cast<void>(restarted.OpenTheory(uid, principle));
  Check(count("theory_opened") == 2, "repeat principle opening was lost");
  const boost::json::object reveal{{"session_id", sid}, {"question_id", target->id}, {"kind", "answer"}};
  static_cast<void>(restarted.OpenTheory(uid, reveal));
  static_cast<void>(restarted.OpenTheory(uid, reveal));
  Check(count("answer_revealed") == 2, "repeat answer disclosure was lost");
  static_cast<void>(restarted.CurrentSession(uid));
  Check(count("answer_revealed") == 3, "restored visible answer was not journalled");
  const auto after_help = repository.LoadProgressSummary(uid);
  Check(after_help.xp_total == before_help.xp_total && after_help.answers.attempts == before_help.answers.attempts &&
            after_help.completed_count == before_help.completed_count &&
            repository.LoadCleanQuestionFamilies(uid).empty(),
        "content delivery or help created scored learning evidence");
  Check(Sql(db.get(), "SELECT bool_and(question_id='" + target->id + "' AND question_content_revision='" +
                          target->content_revision +
                          "' AND attempt_number=1 AND offered_at >= "
                          "(SELECT started_at FROM study_contact_tracking)) FROM study_contacts WHERE " +
                          filter) == "t",
        "journal lost pinned revision, attempt or tracking boundary");
  Check(Sql(db.get(), "SELECT max(offered_at)>min(offered_at) FROM study_contacts WHERE " + filter) == "t",
        "separate requests reused the first-opening timestamp");
  auto invalid = help;
  invalid["question_id"] = "not-the-current-card";
  const auto before_rejection = Sql(db.get(), "SELECT count(*) FROM study_contacts WHERE " + filter);
  try {
    static_cast<void>(restarted.OpenReference(uid, invalid));
    throw std::runtime_error("invalid help accepted");
  } catch (const std::logic_error&) {}
  Check(Sql(db.get(), "SELECT count(*) FROM study_contacts WHERE " + filter) == before_rejection,
        "invalid help wrote a contact");
  const auto current = repository.LoadActiveTrainingSession(uid);
  const StudyContactContext contact{target->id, target->content_revision, 1, "answer_revealed"};
  Reject([&] { repository.RecordStudyContact(uid + 1, sid, current->state_version, contact); });
  Reject([&] { repository.RecordStudyContact(uid, sid, current->state_version - 1, contact); });
  auto wrong_context = contact;
  wrong_context.question_content_revision = std::string_view{};
  Reject([&] { repository.RecordStudyContact(uid, sid, current->state_version, wrong_context); });
  wrong_context = contact;
  wrong_context.kind = "question_delivered";
  Reject([&] { repository.RecordStudyContact(uid, sid, current->state_version, wrong_context); });
  Check(Sql(db.get(), "SELECT count(*) FROM study_contacts WHERE " + filter) == before_rejection,
        "unowned, stale, revisionless or wrong-phase contact was accepted");
  auto legacy = boost::json::parse(current->state_json).as_object();
  legacy.at("last_feedback").as_object().erase("content_revision");
  static_cast<void>(repository.UpdateActiveTrainingSession(uid, sid, current->state_version, current->content_version,
                                                           boost::json::serialize(legacy)));
  MiniAppService legacy_service(bank, pool);
  static_cast<void>(legacy_service.CurrentSession(uid));
  Check(Sql(db.get(), "SELECT question_content_revision IS NULL FROM study_contacts WHERE " + filter +
                          " ORDER BY id DESC LIMIT 1") == "t",
        "revisionless legacy feedback was silently attributed to today's content");

  // A content update can preserve option IDs while changing the actual question.
  // Restoring that old unanswered snapshot must not serve or grade the new text
  // under the old revision; preserve the old training rather than lose it.
  const auto stale_uid = uid + 1;
  const auto old_card = service.StartSession(
      stale_uid,
      {{"modes", boost::json::array{"code"}}, {"topics", boost::json::array{target->topic}}, {"level", target->level}});
  const auto old_sid = old_card.at("session_id").as_int64();
  const auto old_snapshot = repository.LoadActiveTrainingSession(stale_uid);
  auto changed_content = boost::json::parse(target->content_snapshot_json).as_object();
  changed_content["text"] = "A different statement with the same option IDs, only for the isolated test.";
  std::vector<Question> changed_cards;
  for (const auto& id : bank.QuestionIds())
    changed_cards.push_back(id == target->id ? ParseQuestionContent(boost::json::serialize(changed_content))
                                             : bank.Get(id));
  QuestionBank changed_bank(std::move(changed_cards));
  MiniAppService changed_service(changed_bank, pool);
  Reject([&] { static_cast<void>(changed_service.CurrentSession(stale_uid)); });
  Reject([&] {
    static_cast<void>(
        changed_service.SubmitAnswer(stale_uid, {{"question_id", target->id}, {"answer", task.correct_option_id}}));
  });
  Check(Sql(db.get(), "SELECT count(*) FROM study_contacts WHERE session_id=" + std::to_string(old_sid)) == "1" &&
            Sql(db.get(), "SELECT count(*) FROM attempts WHERE session_id=" + std::to_string(old_sid)) == "0",
        "restored changed content created a false contact or graded answer");
  const auto retained = repository.LoadSavedTrainingSessions(stale_uid);
  Check(retained.size() == 1 && retained.front().session_id == old_sid &&
            retained.front().state_json == old_snapshot->state_json,
        "a changed card discarded the unfinished training");
  const TelegramMiniAppUser learner{
      .id = stale_uid, .first_name = {}, .last_name = {}, .username = {}, .language_code = {}};
  Check(changed_service.Bootstrap(learner).at("session").is_null(),
        "bootstrap offered a changed card from an incompatible training");
}
