#include "learning/reference_catalog.h"
#include "miniapp/miniapp_service.h"
#include "miniapp/session_snapshot.h"
#include "storage/attempt_repository.h"
#include "storage/question_loader.h"

#include <libpq-fe.h>
#include <boost/json.hpp>

#include <algorithm>
#include <chrono>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {
void Check(bool condition, const char* message) {
  if (!condition)
    throw std::runtime_error(message);
}
template <typename F>
void Reject(F&& operation) {
  bool rejected = false;
  try {
    operation();
  } catch (const std::logic_error&) {
    rejected = true;
  }
  Check(rejected, "invalid reference or saved training request was accepted");
}
std::string Text(const boost::json::object& object, std::string_view key) {
  return std::string(object.at(key).as_string());
}

void CheckLegacyRollback(const QuestionBank& bank, const std::shared_ptr<DatabaseConnectionPool>& pool, PGconn* db,
                         std::int64_t uid) {
  AttemptRepository repository(pool);
  MiniAppService service(bank, pool);
  const boost::json::object request{
      {"modes", boost::json::array{"code"}}, {"topics", boost::json::array{}}, {"level", 3}};
  const auto first = service.StartSession(uid, request);
  const auto& question = first.at("question").as_object();
  static_cast<void>(service.OpenReference(
      uid, {{"question_id", Text(question, "id")},
            {"reference_id", Text(question.at("references").as_array().front().as_object(), "id")}}));
  const auto original = repository.LoadActiveTrainingSession(uid);
  static_cast<void>(service.StartSession(uid, request));
  const auto current = repository.LoadActiveTrainingSession(uid);
  const auto owner = std::to_string(uid);

  // Query and destructive replacement from 0f43b45, without is_current or any
  // knowledge of the new archive. The old image must still see exactly one row.
  const auto old_query =
      "SELECT a.session_id FROM active_training_sessions a JOIN sessions s "
      "ON s.id = a.session_id AND s.user_id = a.user_id WHERE a.user_id = " +
      owner + " AND a.expires_at > now() AND s.finished_at IS NULL";
  std::unique_ptr<PGresult, decltype(&PQclear)> loaded(PQexec(db, old_query.c_str()), PQclear);
  Check(loaded && PQresultStatus(loaded.get()) == PGRES_TUPLES_OK && PQntuples(loaded.get()) == 1,
        "the previous binary would see multiple current snapshots after rollback");
  const auto legacy_replace =
      "BEGIN; UPDATE sessions SET finished_at = now(), finish_reason = CASE WHEN EXISTS ("
      "SELECT 1 FROM active_training_sessions a WHERE a.session_id = sessions.id AND a.expires_at <= now()) "
      "THEN 'expired' ELSE 'miniapp_restarted' END WHERE user_id = " +
      owner +
      " AND left(mode, char_length('miniapp:')) = 'miniapp:' AND finished_at IS NULL; "
      "DELETE FROM active_training_sessions WHERE user_id = " +
      owner + "; COMMIT;";
  std::unique_ptr<PGresult, decltype(&PQclear)> replaced(PQexec(db, legacy_replace.c_str()), PQclear);
  Check(replaced && PQresultStatus(replaced.get()) == PGRES_COMMAND_OK, "legacy restart SQL failed");
  Check(repository.LoadSavedTrainingSessions(uid).size() == 2,
        "the previous binary's implicit restart lost unfinished trainings");

  const auto third =
      repository.StartActiveTrainingSession(uid, "miniapp:legacy", current->content_version, current->state_json);
  const auto old_update =
      "UPDATE active_training_sessions SET state_version = state_version + 1 WHERE user_id = " + owner;
  std::unique_ptr<PGresult, decltype(&PQclear)> updated(PQexec(db, old_update.c_str()), PQclear);
  Check(updated && PQresultStatus(updated.get()) == PGRES_COMMAND_OK, "legacy snapshot update failed");
  const auto mirrored_query =
      "SELECT a.state_version = saved.state_version FROM active_training_sessions a "
      "JOIN saved_training_sessions saved USING(session_id) WHERE a.user_id = " +
      owner;
  std::unique_ptr<PGresult, decltype(&PQclear)> mirrored(PQexec(db, mirrored_query.c_str()), PQclear);
  Check(mirrored && PQresultStatus(mirrored.get()) == PGRES_TUPLES_OK && PQntuples(mirrored.get()) == 1 &&
            std::string(PQgetvalue(mirrored.get(), 0, 0)) == "t",
        "legacy writes bypassed the snapshot mirror");
  MiniAppService forward_again(bank, pool);
  const auto resumed = forward_again.ResumeSession(uid, {{"session_id", original->session_id}});
  Check(resumed.at("question").as_object().at("assisted").as_bool(), "rollback lost the original assistance marker");
  static_cast<void>(forward_again.StopSession(uid));
  repository.FinishSession(current->session_id, "miniapp_stopped");
  repository.FinishSession(third, "miniapp_stopped");
  Check(repository.LoadSavedTrainingSessions(uid).empty(), "explicit completion retained hidden unfinished entries");
}
void CheckRealCardReferenceHelp(const char* url, PGconn* db, std::int64_t first_user) {
  const auto all = LoadQuestions("content/questions");
  auto pool = std::make_shared<DatabaseConnectionPool>(url, 3);
  AttemptRepository repository(pool);
  const std::vector<std::pair<std::string, std::string>> cases{
      {"pointer_after_local_lifetime", "object_lifetime"},
      {"c_str_from_temporary_dangles", "string_c_str"},
      {"unique_ptr_move_transfers_ownership", "unique_ownership"},
      {"unique_ptr_borrow_without_transfer", "unique_ownership"},
      {"vector_raii_early_return", "scope_cleanup"},
      {"raii_lock_guard_unlocks_during_unwinding", "scope_cleanup"},
      {"vector_resize_adds_elements", "vector_storage"},
      {"find_missing_end_guard", "iterator_range"},
      {"vector_erase_return_next", "vector_iterators"},
      {"map_find_without_insertion", "map_lookup"},
      {"map_insert_or_assign_vs_try_emplace", "map_update"},
      {"map_update_existing_stock", "map_update"},
      {"oop_vector_base_slices_derived", "object_slicing"},
      {"oop_virtual_dispatch_basic", "virtual_dispatch"},
      {"oop_pure_virtual_destructor_definition", "virtual_destruction"},
  };
  for (const auto& [id, reference] : cases) {
    const auto target = std::ranges::find(all, id, &Question::id);
    Check(target != all.end(), "real-card reference fixture missing");
    // Four real cards in other topics satisfy the existing minimum selectable
    // level. The requested topic contains only the target, without fake metadata.
    std::vector<Question> cards{*target};
    for (const auto& card : all) {
      if (cards.size() == 5) {
        break;
      }
      if (card.level == target->level && card.topic != target->topic) {
        cards.push_back(card);
      }
    }
    Check(cards.size() == 5, "not enough cross-topic fixtures for selectable level");
    QuestionBank bank(std::move(cards));
    const auto uid = first_user++;
    repository.EnsureUser(uid);
    // Higher-level real cards need an appropriately unlocked test user; never
    // weaken the application's difficulty gate to exercise help.
    const auto level = std::to_string(target->level);
    const auto update = "UPDATE users SET adaptive_level = " + level + ", unlocked_level = " + level +
                        " WHERE user_id = " + std::to_string(uid);
    std::unique_ptr<PGresult, decltype(&PQclear)> updated(PQexec(db, update.c_str()), PQclear);
    Check(updated && PQresultStatus(updated.get()) == PGRES_COMMAND_OK, "cannot prepare reference user's level");
    MiniAppService service(bank, pool);
    const auto first = service.StartSession(uid, {{"modes", boost::json::array{"code"}},
                                                  {"topics", boost::json::array{target->topic}},
                                                  {"level", target->level}});
    Check(Text(first.at("question").as_object(), "id") == id, "unexpected real-card fixture selected");
    const auto snapshot = repository.LoadActiveTrainingSession(uid);
    // Existing catalog articles must not be accessible simply because the code
    // mentions a vector. Reject before persisting assistance or changing XP.
    if (id == "oop_vector_base_slices_derived") {
      for (const auto unrelated : {"vector_storage", "vector_iterators", "unknown-reference"}) {
        Reject(
            [&] { static_cast<void>(service.OpenReference(uid, {{"question_id", id}, {"reference_id", unrelated}})); });
        const auto after = repository.LoadActiveTrainingSession(uid);
        Check(after->state_version == snapshot->state_version && after->state_json == snapshot->state_json &&
                  !service.CurrentSession(uid).at("question").as_object().at("assisted").as_bool(),
              "an unrelated or unknown article consumed independent-answer eligibility");
      }
      MiniAppService after_rejection(bank, pool);
      Check(!after_rejection.CurrentSession(uid).at("question").as_object().at("assisted").as_bool(),
            "a rejected article persisted assistance across restart");
    }
    const boost::json::object request{{"question_id", id}, {"reference_id", reference}};
    const auto help = service.OpenReference(uid, request);
    Check(help.at("assisted").as_bool(), "reference was disclosed without marking help");
    MiniAppService restarted(bank, pool);
    Check(restarted.CurrentSession(uid).at("question").as_object().at("assisted").as_bool(),
          "reference help disappeared after restart");
    const auto answer = std::get<SingleChoiceTask>(target->task).correct_option_id;
    const auto receipt = restarted.SubmitAnswer(uid, {{"question_id", id}, {"answer", answer}});
    const auto& feedback = receipt.at("session").as_object().at("feedback").as_object();
    Check(feedback.at("assisted").as_bool() && feedback.at("xp_gained").as_int64() == 0,
          "reference-assisted answer earned XP");
    Check(repository.LoadCleanQuestionFamilies(uid).empty(), "lifetime help gave independent evidence");
    Check(repository.LoadProgressSummary(uid).answers.correct_first_answers == 0,
          "lifetime help counted as a clean first answer");
    static_cast<void>(restarted.StopSession(uid));
  }
}
void CheckLearningRouteSession(const char* url, std::int64_t uid) {
  QuestionBank bank(LoadQuestions("content/questions"));
  auto pool = std::make_shared<DatabaseConnectionPool>(url, 3);
  AttemptRepository repository(pool);
  MiniAppService service(bank, pool);
  const auto prior = service.StartSession(
      uid, {{"modes", boost::json::array{"code"}}, {"topics", boost::json::array{"containers"}}, {"level", 2}});
  Reject([&] { static_cast<void>(service.StartSession(uid, {{"route_id", "unknown"}})); });
  Reject([&] { static_cast<void>(service.StartSession(uid, {{"route_id", "containers_iterators"}, {"level", 10}})); });
  const boost::json::object request{{"route_id", "containers_iterators"},
                                    {"request_id", "00000000-0000-4000-8000-000000000123"}};
  const auto first = service.StartSession(uid, request);
  Check(service.StartSession(uid, request).at("session_id") == first.at("session_id"),
        "duplicate route start created a new training");
  Check(service.StartSession(uid, {{"route_id", "containers_iterators"}}).at("session_id") == first.at("session_id"),
        "starting an active route again created a duplicate");
  Check(Text(first.at("question").as_object(), "id") == "vector_reserve_does_not_change_size",
        "route did not start with the introduction");
  const auto saved = repository.LoadSavedTrainingSessions(uid);
  Check(saved.size() == 1 && saved.front().session_id == prior.at("session_id").as_int64(),
        "route start destroyed earlier training");
  static_cast<void>(service.OpenReference(
      uid, {{"question_id", "vector_reserve_does_not_change_size"}, {"reference_id", "vector_storage"}}));
  static_cast<void>(
      service.SubmitAnswer(uid, {{"question_id", "vector_reserve_does_not_change_size"}, {"answer", "a"}}));
  const auto next = service.NextQuestion(uid);
  Check(Text(next.at("question").as_object(), "id") == "vector_resize_adds_elements", "route order was shuffled");
  MiniAppService restarted(bank, pool);
  Check(Text(restarted.CurrentSession(uid), "learning_route_id") == "containers_iterators",
        "route identity disappeared after restart");
  const auto stored = repository.LoadActiveTrainingSession(uid);
  auto legacy = boost::json::parse(stored->state_json).as_object();
  legacy.erase("learning_route_id");
  Check(ParseMiniAppSessionSnapshot(boost::json::serialize(legacy), stored->session_id, stored->state_version, bank)
            .learning_route_id.empty(),
        "an old snapshot without route metadata became incompatible");
  Check(Text(restarted.CurrentSession(uid).at("question").as_object(), "id") == "vector_resize_adds_elements",
        "route progress disappeared after restart");
  const auto previous_id = prior.at("session_id").as_int64();
  static_cast<void>(restarted.ResumeSession(uid, {{"session_id", previous_id}}));
  const auto resumed = restarted.ResumeSession(uid, {{"session_id", first.at("session_id")}});
  Check(Text(resumed.at("question").as_object(), "id") == "vector_resize_adds_elements",
        "switching trainings discarded the route");
  const auto map = restarted.StartSession(uid, {{"route_id", "map_lookup"}});
  Check(Text(map, "learning_route_id") == "map_lookup" &&
            Text(map.at("question").as_object(), "id") == "map_find_without_insertion",
        "server did not start the catalog's second route");
  MiniAppService map_restart(bank, pool);
  Check(Text(map_restart.CurrentSession(uid), "learning_route_id") == "map_lookup",
        "second route failed snapshot validation");
  const auto older_route = map_restart.ResumeSession(uid, {{"session_id", first.at("session_id")}});
  Check(Text(older_route.at("question").as_object(), "id") == "vector_resize_adds_elements",
        "starting another catalog route destroyed the first one");
}
}  // namespace

void CheckReferenceHelp(const char* url) {
  auto questions = LoadQuestions("content/questions");
  std::erase_if(questions, [](const Question& q) { return q.public_id < "CR-000157"; });
  QuestionBank bank(std::move(questions));
  auto pool = std::make_shared<DatabaseConnectionPool>(url, 3);
  AttemptRepository repository(pool);
  const auto uid = 3'000'000'000 + std::chrono::steady_clock::now().time_since_epoch().count() % 1'000'000'000;
  MiniAppService service(bank, pool);
  const auto first = service.StartSession(
      uid, {{"modes", boost::json::array{"code"}}, {"topics", boost::json::array{}}, {"level", 3}});
  const auto& question = first.at("question").as_object();
  const auto id = Text(question, "id");
  const auto reference = Text(question.at("references").as_array().front().as_object(), "id");
  const auto before = repository.LoadActiveTrainingSession(uid);
  const auto adaptive = repository.GetAdaptiveState(uid);
  Reject([&] { static_cast<void>(service.OpenReference(uid, {{"question_id", id}, {"reference_id", "unknown"}})); });
  Reject([&] {
    static_cast<void>(service.OpenReference(uid, {{"question_id", "another"}, {"reference_id", reference}}));
  });
  Check(repository.LoadActiveTrainingSession(uid)->state_version == before->state_version,
        "invalid help changed the session");
  const boost::json::object help_request{{"question_id", id}, {"reference_id", reference}};
  const auto help = service.OpenReference(uid, help_request);
  const auto helped = repository.LoadActiveTrainingSession(uid);
  Check(help.at("assisted").as_bool() && !help.at("reference").as_object().contains("correct_option_id"),
        "help leaked a grading key");
  Check(service.OpenReference(uid, help_request) == help &&
            repository.LoadActiveTrainingSession(uid)->state_version == helped->state_version,
        "reopening help was not idempotent");
  MiniAppService restored(bank, pool);
  Check(restored.CurrentSession(uid).at("question").as_object().at("assisted").as_bool(), "restart erased assistance");
  const auto answer = std::get<SingleChoiceTask>(bank.Get(id).task).correct_option_id;
  const boost::json::object answer_request{{"question_id", id}, {"answer", answer}};
  const auto receipt = restored.SubmitAnswer(uid, answer_request);
  const auto& feedback = receipt.at("session").as_object().at("feedback").as_object();
  Check(feedback.at("assisted").as_bool() && feedback.at("xp_gained").as_int64() == 0, "assisted answer earned XP");
  Check(restored.SubmitAnswer(uid, answer_request) == receipt, "assisted receipt replay changed the result");
  Check(repository.GetAdaptiveState(uid).evidence == adaptive.evidence, "help raised adaptive evidence");
  const auto progress = repository.LoadProgressSummary(uid);
  Check(progress.first_try_correct_count == 0 && progress.answers.correct_first_answers == 0,
        "help counted as independent first-answer success");
  Check(repository.LoadCleanQuestionFamilies(uid).empty(), "help supplied clean family evidence");
  Check(repository.LoadReviewStates(uid).at(id).stage == 0, "help advanced the repetition stage");
  Reject([&] { static_cast<void>(restored.OpenReference(uid, help_request)); });

  const auto first_saved = repository.LoadActiveTrainingSession(uid);
  static_cast<void>(restored.StartSession(
      uid, {{"modes", boost::json::array{"code"}}, {"topics", boost::json::array{}}, {"level", 3}}));
  const auto second = repository.LoadActiveTrainingSession(uid);
  Reject([&] { static_cast<void>(restored.StopSession(uid, {{"session_id", first_saved->session_id}})); });
  Reject([&] { static_cast<void>(restored.NextQuestion(uid, {{"session_id", first_saved->session_id}})); });
  const auto saved = repository.LoadSavedTrainingSessions(uid);
  Check(saved.size() == 1 && saved.front().session_id == first_saved->session_id &&
            saved.front().state_json == first_saved->state_json,
        "starting a new training lost or changed the unfinished snapshot");
  Reject([&] {
    static_cast<void>(repository.UpdateActiveTrainingSession(uid, first_saved->session_id, first_saved->state_version,
                                                             first_saved->content_version, first_saved->state_json));
  });
  Reject([&] { static_cast<void>(restored.ResumeSession(uid + 1, {{"session_id", first_saved->session_id}})); });
  std::unique_ptr<PGconn, decltype(&PQfinish)> db(PQconnectdb(url), PQfinish);
  const auto update = "UPDATE users SET adaptive_level = 4, unlocked_level = 4 WHERE user_id = " + std::to_string(uid);
  std::unique_ptr<PGresult, decltype(&PQclear)> updated(PQexec(db.get(), update.c_str()), PQclear);
  Check(updated && PQresultStatus(updated.get()) == PGRES_COMMAND_OK, "cannot prepare changed progress fixture");
  MiniAppService restarted(bank, pool);
  const auto resumed = restarted.ResumeSession(uid, {{"session_id", first_saved->session_id}});
  Check(resumed.at("progress").as_object().at("adaptive_level").as_int64() == 4,
        "resuming an old training rolled back progress earned elsewhere");
  Check(resumed.at("feedback") == receipt.at("session").as_object().at("feedback"),
        "saved feedback or assistance was lost");
  Check(repository.LoadSavedTrainingSessions(uid).front().session_id == second->session_id,
        "resuming one training discarded the other");
  static_cast<void>(restarted.StopSession(uid));
  Check(repository.LoadSavedTrainingSessions(uid).size() == 1, "explicitly finishing one training deleted another");
  const auto other = restarted.ResumeSession(uid, {{"session_id", second->session_id}});
  Check(other.at("phase").as_string() == "question", "saved unanswered training did not resume");
  static_cast<void>(restarted.StopSession(uid));
  const auto event_sql =
      "SELECT count(*) FROM learning_events WHERE event_name = 'reference_opened' AND user_id = " + std::to_string(uid);
  std::unique_ptr<PGresult, decltype(&PQclear)> events(PQexec(db.get(), event_sql.c_str()), PQclear);
  Check(events && PQresultStatus(events.get()) == PGRES_TUPLES_OK && std::string(PQgetvalue(events.get(), 0, 0)) == "1",
        "the immutable reference event was lost or duplicated after stopping");
  const auto incompatible_id = repository.StartActiveTrainingSession(uid, "miniapp:old-content", "old-content", "{}");
  MiniAppService incompatible(bank, pool);
  Reject([&] { static_cast<void>(incompatible.CurrentSession(uid)); });
  const auto unavailable = repository.LoadSavedTrainingSessions(uid);
  Check(!repository.LoadActiveTrainingSession(uid) && unavailable.size() == 1 &&
            unavailable.front().session_id == incompatible_id && unavailable.front().state_json == "{}",
        "an incompatible unfinished snapshot was silently deleted");
  CheckLegacyRollback(bank, pool, db.get(), uid + 2'000'000'000);
  CheckRealCardReferenceHelp(url, db.get(), uid + 4'000'000'000);
  CheckLearningRouteSession(url, uid + 5'000'000'000);
}
