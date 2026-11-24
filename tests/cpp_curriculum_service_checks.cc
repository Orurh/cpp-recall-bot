#include "learning/cpp_curriculum.h"
#include "learning/reference_catalog.h"
#include "miniapp/miniapp_service.h"
#include "miniapp/session_snapshot.h"
#include "storage/attempt_repository.h"
#include "storage/question_loader.h"

#include <libpq-fe.h>
#include <boost/json.hpp>

#include <algorithm>
#include <chrono>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>

namespace {
void Require(bool condition, std::string_view message) {
  if (!condition)
    throw std::runtime_error(std::string(message));
}

template <typename Error, typename Operation>
void Denied(Operation&& operation, std::string_view message) {
  bool denied = false;
  try {
    operation();
  } catch (const Error&) {
    denied = true;
  }
  Require(denied, message);
}

void MetadataOnly(const boost::json::value& value) {
  if (value.is_array()) {
    for (const auto& child : value.as_array())
      MetadataOnly(child);
  } else if (value.is_object()) {
    for (const auto& field : value.as_object()) {
      for (const auto key : {"answer", "accepted_answers", "correct_option_id", "explanation", "example", "task",
                             "code", "principle", "mechanism", "sections"})
        Require(field.key() != key, "curriculum metadata leaked an answer or help body");
      MetadataOnly(field.value());
    }
  }
}
}  // namespace

void CheckCppCurriculumService(const char* url) {
  using boost::json::object;
  const auto uid = 32'000'000'000 + std::chrono::steady_clock::now().time_since_epoch().count() % 1'000'000'000;
  auto pool = std::make_shared<DatabaseConnectionPool>(url, 3);
  AttemptRepository repository(pool);
  repository.EnsureUser(uid);
  std::unique_ptr<PGconn, decltype(&PQfinish)> connection(PQconnectdb(url), PQfinish);
  Require(connection && PQstatus(connection.get()) == CONNECTION_OK, "curriculum fixture DB unavailable");
  const auto sql = [&](const std::string& query) -> std::string {
    std::unique_ptr<PGresult, decltype(&PQclear)> result(PQexec(connection.get(), query.c_str()), PQclear);
    Require(
        result && (PQresultStatus(result.get()) == PGRES_TUPLES_OK || PQresultStatus(result.get()) == PGRES_COMMAND_OK),
        "curriculum fixture SQL failed");
    return PQntuples(result.get()) == 0 ? "" : PQgetvalue(result.get(), 0, 0);
  };
  const auto snapshot = [&] {
    std::string state;
    for (const auto table :
         {"users", "sessions", "attempts", "question_progress", "topic_progress", "skill_progress", "skill_evidence",
          "user_skill_state", "xp_ledger", "active_training_sessions", "saved_training_sessions",
          "daily_practice_assignments", "user_daily_settings", "study_contacts", "reference_views", "learning_events"})
      state += sql("SELECT COALESCE(jsonb_agg(to_jsonb(t) ORDER BY to_jsonb(t)::text),'[]')::text FROM " +
                   std::string(table) + " t WHERE user_id=" + std::to_string(uid)) +
               "\n";
    return state;
  };
  QuestionBank bank(LoadQuestions("content/questions"));
  MiniAppService service(bank, pool);
  const auto before_metadata = snapshot();
  const auto catalog = service.CppCurriculum(uid, {{"topic", "containers"}});
  MetadataOnly(catalog);
  Require(catalog.at("levels").as_array().size() == 10, "curriculum lost visible levels");
  Require(snapshot() == before_metadata, "curriculum metadata mutated existing learning state");

  constexpr auto first_unit = "cpp.containers.vector.l1";
  constexpr auto second_unit = "cpp.containers.map_lookup.l2";
  const auto material_request = object{{"topic", "containers"}, {"unit_id", first_unit}};
  const auto material = service.CppCurriculum(uid, material_request);
  Require(material.at("unit_id") == first_unit &&
              (!material.at("theories").as_array().empty() || !material.at("references").as_array().empty()),
          "standalone curriculum material missing");
  Require(service.CppCurriculum(uid, material_request) == material && snapshot() == before_metadata,
          "standalone preparation invented XP, mastery, contacts or a second progress store");

  for (const auto& request : {object{{"topic", "containers"}, {"unit_id", "cpp.containers.vector.l10"}},
                              object{{"topic", "containers"}, {"unit_id", "cpp.containers.vector.l999"}},
                              object{{"topic", "containers"}, {"answer", "tampered"}}})
    Denied<std::invalid_argument>([&] { static_cast<void>(service.CppCurriculum(uid, request)); },
                                  "locked/tampered curriculum request accepted");
  constexpr auto request_id = "cc020000-0000-4000-8000-000000000001";
  for (const auto& request :
       {object{{"learning_unit_id", "cpp.containers.vector.l10"}, {"request_id", request_id}},
        object{{"learning_unit_id", first_unit}, {"request_id", request_id}, {"level", 10}},
        object{{"learning_unit_id", first_unit}, {"request_id", request_id}, {"question_ids", boost::json::array{}}},
        object{{"learning_unit_id", second_unit}}})
    Denied<std::invalid_argument>([&] { static_cast<void>(service.StartSession(uid, request)); },
                                  "unit start accepted locked/tampered/non-idempotent request");
  Require(snapshot() == before_metadata, "denied curriculum operation mutated learning state");

  Denied<std::invalid_argument>(
      [&] {
        static_cast<void>(service.StartSession(uid, {{"learning_unit_id", first_unit}, {"request_id", request_id}}));
      },
      "unit bypassed its storage-duration prerequisite");
  Require(snapshot() == before_metadata, "prerequisite denial mutated learning state");
  sql("INSERT INTO user_skill_state(user_id,skill_id,status) VALUES (" + std::to_string(uid) +
      ",'core.storage_duration','demonstrated')");
  sql("INSERT INTO skill_progress(user_id,skill_id,question_id) VALUES (" + std::to_string(uid) +
      ",'core.storage_duration','storage_duration_static_local')");

  const object start{{"learning_unit_id", first_unit}, {"request_id", request_id}};
  const auto session = service.StartSession(uid, start);
  const auto sid = session.at("session_id");
  const auto stored = repository.LoadActiveTrainingSession(uid);
  Require(stored.has_value(), "unit session not stored in ordinary active training");
  const auto retry = service.StartSession(uid, start);
  const auto stored_retry = repository.LoadActiveTrainingSession(uid);
  Require(retry.at("session_id") == sid && retry.at("question") == session.at("question") &&
              stored_retry->state_json == stored->state_json && stored_retry->state_version == stored->state_version,
          "same unit request duplicated or reset the training");
  const auto before_conflict = snapshot();
  Denied<std::logic_error>(
      [&] {
        static_cast<void>(service.StartSession(uid, {{"learning_unit_id", second_unit}, {"request_id", request_id}}));
      },
      "request ID could be reused for another unit");
  Require(snapshot() == before_conflict, "conflicting unit retry mutated learning state");

  const auto assertBlocked = [&](const object& request, const boost::json::value& expected_id) {
    const auto before = snapshot();
    const auto blocked = service.CppCurriculum(uid, request);
    Require(blocked.at("blocked_session_id") == expected_id && !blocked.contains("theories") &&
                !blocked.contains("references"),
            "standalone help leaked into pending training");
    Require(snapshot() == before, "blocked standalone help changed progress or marked assistance");
  };
  assertBlocked(material_request, sid);
  const auto second = service.StartSession(
      uid, {{"learning_unit_id", second_unit}, {"request_id", "cc020000-0000-4000-8000-000000000002"}});
  Require(second.at("session_id") != sid, "different unit reused the old session");
  assertBlocked(material_request, sid);
  const auto resumed = service.StartSession(uid, start);
  Require(resumed.at("session_id") == sid && resumed.at("question") == session.at("question"),
          "retry of parked unit lost session identity or current question");
  assertBlocked({{"topic", "containers"}, {"unit_id", second_unit}}, second.at("session_id"));

  // Different families can share an explanation that teaches the pending answer.
  // Only the L3 write-code map tasks are queued; the L2 unit shares their theory.
  repository.EnsureUser(uid + 1);
  const auto map_session = service.StartSession(
      uid + 1,
      {{"level", 3}, {"topics", boost::json::array{"containers"}}, {"modes", boost::json::array{"write_code"}}});
  const auto map_before = repository.LoadActiveTrainingSession(uid + 1);
  const auto cross_family = service.CppCurriculum(uid + 1, {{"topic", "containers"}, {"unit_id", second_unit}});
  Require(cross_family.at("blocked_session_id") == map_session.at("session_id") && !cross_family.contains("theories") &&
              !cross_family.contains("references"),
          "cross-family shared theory/reference bypassed assistance tracking");
  const auto map_after = repository.LoadActiveTrainingSession(uid + 1);
  Require(map_after->state_json == map_before->state_json && map_after->state_version == map_before->state_version,
          "cross-family blocked help mutated saved training");

  // A planned fourth card is pending even before the adaptive three-card queue
  // has delivered it. Build a persisted ordinary snapshot, not a second state.
  Require(service.CppCurriculum(uid + 1, material_request).contains("theories"),
          "future-card fixture already overlaps the visible map cards");
  auto planned =
      ParseMiniAppSessionSnapshot(map_after->state_json, map_after->session_id, map_after->state_version, bank);
  const auto pending_ids = planned.quiz.queue.ScheduledQuestionIds();
  planned.quiz.training_plan = {};
  for (const auto& id : pending_ids)
    planned.quiz.training_plan.questions.push_back({id, TrainingQuestionPurpose::kNewMaterial});
  planned.quiz.training_plan.questions.push_back({"vector_push_pop_trace", TrainingQuestionPurpose::kNewMaterial});
  planned.quiz.training_plan.new_question_target = planned.quiz.training_plan.questions.size();
  planned.quiz.session_goal = planned.quiz.training_plan.questions.size();
  planned.quiz.selection.min_level = 1;
  planned.quiz.selection.max_level = 3;
  planned.quiz.selection.kinds.clear();
  planned.quiz.selection.skill_ids.clear();
  planned.quiz.selection.preferred_question_ids.clear();
  static_cast<void>(repository.UpdateActiveTrainingSession(uid + 1, map_after->session_id, map_after->state_version,
                                                           kMiniAppSessionContentVersion,
                                                           SerializeMiniAppSessionSnapshot(planned)));
  const auto future_before = repository.LoadActiveTrainingSession(uid + 1);
  MiniAppService restarted(bank, pool);
  const auto future_help = restarted.CppCurriculum(uid + 1, material_request);
  Require(future_help.at("blocked_session_id") == map_session.at("session_id") && !future_help.contains("theories") &&
              !future_help.contains("references"),
          "unqueued planned card received standalone help without assistance tracking");
  const auto future_after = repository.LoadActiveTrainingSession(uid + 1);
  Require(future_before->state_json == future_after->state_json &&
              future_before->state_version == future_after->state_version,
          "unqueued help denial mutated training");

  repository.EnsureUser(uid + 3);
  const auto review_user = std::to_string(uid + 3);
  sql("UPDATE users SET adaptive_level=4,unlocked_level=7 WHERE user_id=" + review_user);
  sql("INSERT INTO topic_progress(user_id,topic,adaptive_level) VALUES (" + review_user + ",'containers',4)");
  sql("INSERT INTO question_progress(user_id,question_id,correct_count,next_review_at) VALUES (" + review_user +
      ",'map_transparent_lookup',1,now()-interval '1 day')");
  const auto due_catalog = service.CppCurriculum(uid + 3, {{"topic", "containers"}});
  bool due_available = false;
  for (const auto& unit : due_catalog.at("levels").as_array()[4].as_object().at("units").as_array())
    if (unit.as_object().at("id") == "cpp.containers.map_lookup.l5")
      due_available = unit.as_object().at("can_practice").as_bool();
  Require(due_available, "demotion closed a known due card despite historical unlocked maximum");
  const auto due_session = service.StartSession(uid + 3, {{"learning_unit_id", "cpp.containers.map_lookup.l5"},
                                                          {"request_id", "cc020000-0000-4000-8000-000000000003"}});
  const auto due_saved = repository.LoadActiveTrainingSession(uid + 3);
  const auto due_state =
      ParseMiniAppSessionSnapshot(due_saved->state_json, due_saved->session_id, due_saved->state_version, bank);
  Require(due_session.at("question").as_object().at("id") == "map_transparent_lookup" &&
              due_state.quiz.selection.preferred_question_ids == std::vector<std::string>{"map_transparent_lookup"} &&
              due_state.quiz.training_plan.new_question_target == 0,
          "review unit introduced an unseen hard card after demotion");

  const auto helped_id = std::string(session.at("question").as_object().at("id").as_string());
  const auto& helped_task = std::get<SingleChoiceTask>(bank.Get(helped_id).task);
  const auto wrong = std::ranges::find_if(
      helped_task.options, [&](const auto& option) { return option.id != helped_task.correct_option_id; });
  Require(wrong != helped_task.options.end(), "missing wrong-answer assistance fixture");
  const auto help_references = ReferencesForQuestion(bank.Get(helped_id));
  Require(!help_references.empty(), "missing reference assistance fixture");
  static_cast<void>(
      service.OpenReference(uid, {{"question_id", helped_id}, {"reference_id", help_references.front()->id}}));
  static_cast<void>(service.SubmitAnswer(uid, {{"question_id", helped_id}, {"answer", wrong->id}}));
  const auto helped = repository.LoadActiveTrainingSession(uid);
  const auto helped_state =
      ParseMiniAppSessionSnapshot(helped->state_json, helped->session_id, helped->state_version, bank);
  static_cast<void>(service.StartSession(
      uid, {{"learning_unit_id", second_unit}, {"request_id", "cc020000-0000-4000-8000-000000000004"}}));
  sql("UPDATE users SET adaptive_level=1 WHERE user_id=" + std::to_string(uid));
  sql("UPDATE user_skill_state SET status='needs_recovery' WHERE user_id=" + std::to_string(uid));
  sql("INSERT INTO topic_progress(user_id,topic,adaptive_level) VALUES (" + std::to_string(uid) +
      ",'containers',1) ON CONFLICT(user_id,topic) DO UPDATE SET adaptive_level=1");
  const auto after_demotion = service.StartSession(uid, start);
  const auto demoted_saved = repository.LoadActiveTrainingSession(uid);
  const auto demoted_state = ParseMiniAppSessionSnapshot(demoted_saved->state_json, demoted_saved->session_id,
                                                         demoted_saved->state_version, bank);
  Require(
      after_demotion.at("session_id") == sid &&
          demoted_state.quiz.selection.preferred_question_ids == helped_state.quiz.selection.preferred_question_ids &&
          demoted_state.reference_viewed_question_ids == helped_state.reference_viewed_question_ids,
      "parked unit retry lost identity or assistance after prerequisite recovery/demotion");

  auto expanded_cards = LoadQuestions("content/questions");
  auto additional = bank.Get("vector_push_pop_trace");
  additional.id = "curriculum_fixture_new_vector_card";
  additional.public_id = "CR-999991";
  expanded_cards.push_back(std::move(additional));
  QuestionBank expanded_bank(std::move(expanded_cards));
  MiniAppService expanded_service(expanded_bank, pool);
  const auto old_retry = expanded_service.StartSession(uid, start);
  const auto expanded_saved = repository.LoadActiveTrainingSession(uid);
  const auto expanded_state = ParseMiniAppSessionSnapshot(expanded_saved->state_json, expanded_saved->session_id,
                                                          expanded_saved->state_version, expanded_bank);
  Require(
      old_retry.at("session_id") == sid &&
          expanded_state.quiz.selection.preferred_question_ids == helped_state.quiz.selection.preferred_question_ids &&
          expanded_state.reference_viewed_question_ids == helped_state.reference_viewed_question_ids,
      "catalog addition rejected or expanded an owned unit retry");
  std::cout << "CONTENT-02 curriculum service PASS: metadata/locks/tampering/idempotency/parked/shared-help/no-XP\n";
}
