#include "learning/reference_catalog.h"
#include "miniapp/miniapp_service.h"
#include "miniapp/session_snapshot.h"
#include "quiz/question_identity.h"
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
#include <string_view>
#include <vector>

namespace {

void Check(bool condition, std::string_view message) {
  if (!condition)
    throw std::runtime_error(std::string(message));
}

template <typename Error, typename Operation>
void MustThrow(Operation&& operation) {
  bool rejected = false;
  try {
    operation();
  } catch (const Error&) {
    rejected = true;
  }
  Check(rejected, "invalid or failed reference opening was accepted");
}

using SqlResult = std::unique_ptr<PGresult, decltype(&PQclear)>;
SqlResult Sql(PGconn* db, const std::string& statement) {
  SqlResult result(PQexec(db, statement.c_str()), PQclear);
  Check(result && (PQresultStatus(result.get()) == PGRES_COMMAND_OK || PQresultStatus(result.get()) == PGRES_TUPLES_OK),
        "provenance fixture SQL failed");
  return result;
}

std::int64_t Count(PGconn* db, const std::string& statement) {
  const auto result = Sql(db, statement);
  Check(PQntuples(result.get()) == 1, "provenance count missing");
  return std::stoll(PQgetvalue(result.get(), 0, 0));
}

boost::json::object Report(const Question& card, std::string request_id) {
  return {
      {"request_id", request_id}, {"question_id", card.id},        {"content_revision", card.content_revision},
      {"phase", "question"},      {"category", "unclear_wording"}, {"comment", "Provenance test, not real feedback."}};
}

boost::json::object Review(MiniAppService& service, std::int64_t admin, std::int64_t report_id) {
  const auto page = service.ListCardFeedback(admin, {{"status", "all"}});
  for (const auto& value : page.at("items").as_array()) {
    const auto& item = value.as_object();
    if (item.at("feedback_id").as_int64() == report_id)
      return item;
  }
  throw std::runtime_error("provenance feedback missing from the admin queue");
}

struct FaultCleanup {
  PGconn* db;
  ~FaultCleanup() {
    PQclear(
        PQexec(db, "DROP TRIGGER IF EXISTS help02_fault ON reference_views; DROP FUNCTION IF EXISTS help02_fault();"));
  }
};

}  // namespace

void CheckReferenceProvenance(const char* url) {
  const auto all = LoadQuestions("content/questions");
  const auto target = std::ranges::find(all, "c_str_from_temporary_dangles", &Question::id);
  Check(target != all.end() && target->level <= 3, "provenance card fixture missing");
  std::vector<Question> cards{*target};
  for (const auto& card : all) {
    if (cards.size() == 5)
      break;
    if (card.level == target->level && card.topic != target->topic)
      cards.push_back(card);
  }
  Check(cards.size() == 5, "provenance cross-topic fixtures missing");
  QuestionBank bank(std::move(cards));
  auto pool = std::make_shared<DatabaseConnectionPool>(url, 3);
  AttemptRepository repository(pool);
  constexpr std::int64_t admin = 4242460;
  MiniAppService service(bank, pool, admin);
  std::unique_ptr<PGconn, decltype(&PQfinish)> db(PQconnectdb(url), PQfinish);
  Check(db && PQstatus(db.get()) == CONNECTION_OK, "provenance database unavailable");
  const auto uid = 6'000'000'000 + std::chrono::steady_clock::now().time_since_epoch().count() % 1'000'000'000;
  const boost::json::object start{
      {"modes", boost::json::array{"code"}}, {"topics", boost::json::array{target->topic}}, {"level", target->level}};
  const auto initial = service.StartSession(uid, start);
  const auto sid = initial.at("session_id").as_int64();
  Check(initial.at("question").as_object().at("id").as_string() == target->id, "unexpected provenance card");
  const auto views = [&] {
    return repository.LoadReferenceViews(uid, sid, target->id, target->content_revision);
  };
  Check(views().empty(), "a reference was recorded before it was opened");
  const auto before_request = Report(*target, "90000000-0000-4000-8000-000000000001");
  const auto before = service.SubmitCardFeedback(uid, before_request);
  Check(Review(service, admin, before.at("feedback_id").as_int64()).at("references").as_array().empty(),
        "unopened help attached to feedback");
  const auto unassisted = repository.LoadActiveTrainingSession(uid);
  MustThrow<std::logic_error>([&] {
    static_cast<void>(service.OpenReference(uid, {{"question_id", target->id}, {"reference_id", "vector_storage"}}));
  });
  Check(views().empty() && repository.LoadActiveTrainingSession(uid)->state_json == unassisted->state_json,
        "rejected help changed provenance or assistance");

  // This instance cached the session before the write; it models a retry after
  // the first HTTP response/commit acknowledgement did not reach the caller.
  MiniAppService stale(bank, pool, admin);
  static_cast<void>(stale.CurrentSession(uid));
  const boost::json::object first_request{{"question_id", target->id}, {"reference_id", "object_lifetime"}};
  const auto help = service.OpenReference(uid, first_request);
  const auto first_snapshot = repository.LoadActiveTrainingSession(uid);
  const auto first = views();
  Check(first.size() == 1 && first.front().reference_id == "object_lifetime" && first.front().attempt_number == 1 &&
            !first.front().opened_at.empty(),
        "first reference identity/time missing");
  const auto first_content = boost::json::parse(first.front().reference_snapshot_json);
  Check(first_content == help.at("reference") &&
            QuestionContentRevision(CanonicalQuestionJson(first_content)) == first.front().reference_content_revision &&
            help.at("reference_content_revision").as_string() == first.front().reference_content_revision &&
            help.at("question_content_revision").as_string() == target->content_revision,
        "archived help differs from what the API disclosed");
  Check(stale.OpenReference(uid, first_request) == help && views().front().id == first.front().id &&
            repository.LoadActiveTrainingSession(uid)->state_version == first_snapshot->state_version,
        "stale-instance retry duplicated the view or forgot assistance");
  Check(repository.LoadReferenceViews(uid + 100, sid, target->id, target->content_revision).empty() &&
            repository.LoadReferenceViews(uid, sid + 100, target->id, target->content_revision).empty() &&
            repository.LoadReferenceViews(uid, sid, "other-question", target->content_revision).empty() &&
            repository.LoadReferenceViews(uid, sid, target->id, std::string(64, '0')).empty(),
        "reference history leaked across owner, session, card or revision");
  const auto during = service.SubmitCardFeedback(uid, Report(*target, "90000000-0000-4000-8000-000000000002"));
  const auto frozen = Review(service, admin, during.at("feedback_id").as_int64()).at("references");
  Check(frozen.as_array().size() == 1 && frozen.as_array().front().as_object().at("content") == first_content,
        "feedback lost the exact opened article");
  static_cast<void>(service.OpenReference(uid, {{"question_id", target->id}, {"reference_id", "string_c_str"}}));
  Check(views().size() == 2, "second reference on an assisted card was not recorded");

  // Separate content editions remain separate records; changing the catalog must
  // never replace the historical text. This fixture is not production material.
  auto changed = *std::ranges::find(ReferenceCatalog(), "object_lifetime", &ReferenceEntry::id);
  changed.mechanism = "Changed only in the isolated provenance test.";
  const auto changed_content = ReferenceContentSnapshot(changed);
  const auto changed_revision = ReferenceContentRevision(changed);
  auto snapshot = repository.LoadActiveTrainingSession(uid);
  static_cast<void>(repository.UpdateActiveTrainingSession(
      uid, sid, snapshot->state_version, snapshot->content_version, snapshot->state_json, std::nullopt,
      ReferenceViewContext{target->id, target->content_revision, changed.id, changed_revision, changed_content, 1}));
  Check(views().size() == 3 && views().front().reference_snapshot_json == first.front().reference_snapshot_json,
        "a new article revision overwrote historical text");
  Check(service.OpenReference(uid, first_request) == help, "retry returned another archived edition");
  Check(Review(service, admin, during.at("feedback_id").as_int64()).at("references") == frozen &&
            service.SubmitCardFeedback(uid, before_request) == before &&
            Review(service, admin, before.at("feedback_id").as_int64()).at("references").as_array().empty(),
        "later help changed an earlier report or its retry");
  Check(Count(db.get(), "SELECT count(*) FROM learning_events WHERE event_name = 'reference_opened' AND session_id = " +
                            std::to_string(sid)) == 1,
        "article count changed the per-card assistance metric");
  MustThrow<FeedbackAccessDenied>([&] { static_cast<void>(service.ListCardFeedback(uid, {{"status", "all"}})); });

  const auto answer = std::get<SingleChoiceTask>(target->task).correct_option_id;
  const auto receipt = service.SubmitAnswer(uid, {{"question_id", target->id}, {"answer", answer}});
  Check(receipt.at("session").as_object().at("feedback").as_object().at("xp_gained").as_int64() == 0 &&
            repository.LoadCleanQuestionFamilies(uid).empty(),
        "provenance changed assisted XP/mastery rules");
  static_cast<void>(service.StopSession(uid));
  Check(views().size() == 3, "finishing a session deleted help provenance");

  const auto legacy_uid = uid + 1;
  static_cast<void>(service.StartSession(legacy_uid, start));
  const auto legacy = repository.LoadActiveTrainingSession(legacy_uid);
  auto state = boost::json::parse(legacy->state_json).as_object();
  state["reference_viewed_question_ids"] = boost::json::array{target->id};
  static_cast<void>(repository.UpdateActiveTrainingSession(legacy_uid, legacy->session_id, legacy->state_version,
                                                           legacy->content_version, boost::json::serialize(state)));
  MiniAppService upgraded(bank, pool, admin);
  Check(upgraded.CurrentSession(legacy_uid).at("question").as_object().at("assisted").as_bool() &&
            repository.LoadReferenceViews(legacy_uid, legacy->session_id, target->id, target->content_revision).empty(),
        "legacy assistance was erased or an article identity was fabricated");
  static_cast<void>(upgraded.OpenReference(legacy_uid, first_request));
  Check(repository.LoadReferenceViews(legacy_uid, legacy->session_id, target->id, target->content_revision).size() == 1,
        "an actual post-upgrade opening of legacy-assisted help was not recorded");

  const auto fault_uid = uid + 2;
  static_cast<void>(service.StartSession(fault_uid, start));
  const auto untouched = repository.LoadActiveTrainingSession(fault_uid);
  {
    FaultCleanup cleanup{db.get()};
    static_cast<void>(Sql(db.get(),
                          "CREATE FUNCTION help02_fault() RETURNS trigger LANGUAGE plpgsql AS $$ BEGIN "
                          "IF NEW.user_id = " +
                              std::to_string(fault_uid) +
                              " THEN RAISE EXCEPTION 'help02 atomic failure'; END IF; "
                              "RETURN NEW; END $$; CREATE TRIGGER help02_fault BEFORE INSERT ON reference_views "
                              "FOR EACH ROW EXECUTE FUNCTION help02_fault();"));
    MustThrow<std::runtime_error>([&] { static_cast<void>(service.OpenReference(fault_uid, first_request)); });
    const auto unchanged = repository.LoadActiveTrainingSession(fault_uid);
    Check(unchanged->state_version == untouched->state_version && unchanged->state_json == untouched->state_json &&
              repository.LoadReferenceViews(fault_uid, untouched->session_id, target->id, target->content_revision)
                  .empty(),
          "a failed provenance write consumed assistance or advanced the session");
    Check(
        Count(db.get(), "SELECT count(*) FROM learning_events WHERE event_name = 'reference_opened' AND session_id = " +
                            std::to_string(untouched->session_id)) == 0,
        "failed help left a partial analytics event");
    auto pending = boost::json::parse(untouched->state_json).as_object();
    pending["reference_viewed_question_ids"] = boost::json::array{target->id};
    const auto pending_json = boost::json::serialize(pending);
    const std::string unique_mechanism = "Rollback-only fixture " + std::to_string(fault_uid);
    auto rollback_article = changed;
    rollback_article.mechanism = unique_mechanism;
    const auto rollback_content = ReferenceContentSnapshot(rollback_article);
    const auto rollback_revision = ReferenceContentRevision(rollback_article);
    const auto persist = [&](std::string_view hash) {
      static_cast<void>(repository.UpdateActiveTrainingSession(
          fault_uid, untouched->session_id, untouched->state_version, untouched->content_version, pending_json,
          std::nullopt,
          ReferenceViewContext{target->id, target->content_revision, rollback_article.id, hash, rollback_content, 1}));
    };
    MustThrow<std::invalid_argument>([&] { persist(std::string(64, '0')); });
    MustThrow<std::runtime_error>([&] { persist(rollback_revision); });
    Check(Count(db.get(), "SELECT count(*) FROM reference_content_revisions WHERE content_revision = '" +
                              rollback_revision + "'") == 0,
          "failed first opening left a partially archived article");
  }
  auto opening = [&] {
    return service.OpenReference(fault_uid, first_request);
  };
  auto a = std::async(std::launch::async, opening);
  auto b = std::async(std::launch::async, opening);
  Check(a.get() == b.get() &&
            repository.LoadReferenceViews(fault_uid, untouched->session_id, target->id, target->content_revision)
                    .size() == 1,
        "concurrent retries duplicated the first opening");
}
