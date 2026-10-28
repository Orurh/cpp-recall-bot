#include "miniapp/miniapp_service.h"
#include "miniapp/session_snapshot.h"
#include "miniapp/shared_challenge.h"
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
  Check(rejected, "an invalid shared challenge operation was accepted");
}
void Sql(PGconn* connection, const std::string& sql) {
  std::unique_ptr<PGresult, decltype(&PQclear)> result(PQexec(connection, sql.c_str()), PQclear);
  Check(result && PQresultStatus(result.get()) == PGRES_COMMAND_OK, "shared challenge fixture SQL failed");
}
}  // namespace

void CheckSharedChallenges(const char* url) {
  const QuestionBank bank(LoadQuestions("content/questions"));
  auto pool = std::make_shared<DatabaseConnectionPool>(url, 3);
  AttemptRepository repository(pool);
  MiniAppService service(bank, pool, 424242, "cpp_recall_bot");
  const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
  const auto uid = 4'000'000'000 + stamp % 1'000'000'000;
  const TelegramMiniAppUser user{.id = uid, .first_name = "Demo", .last_name = {}, .username = {}, .language_code = {}};
  const TelegramMiniAppUser beginner{
      .id = uid + 1, .first_name = "Demo", .last_name = {}, .username = {}, .language_code = {}};
  std::unique_ptr<PGconn, decltype(&PQfinish)> connection(PQconnectdb(url), PQfinish);
  Check(connection && PQstatus(connection.get()) == CONNECTION_OK, "shared challenge fixture database unavailable");

  static_cast<void>(service.Bootstrap(user));
  const auto old = service.StartSession(uid, {{"route_id", "containers_iterators"}});
  const auto old_id = old.at("session_id");
  const auto old_question = old.at("question").as_object();
  const auto old_qid = Text(old_question, "id");
  static_cast<void>(service.OpenReference(
      uid, {{"question_id", old_qid},
            {"reference_id", old_question.at("references").as_array().front().as_object().at("id")}}));
  const auto original_snapshot = repository.LoadActiveTrainingSession(uid);
  const auto original_xp = repository.LoadProgressSummary(uid).xp_total;
  Reject([&] {
    static_cast<void>(service.ShareChallenge(
        uid, {{"question_id", old_qid}, {"content_revision", old_question.at("content_revision")}}));
  });
  const auto launch_time =
      std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
  const auto before = boost::json::parse(repository.SharedChallengeStatistics(1)).as_object();
  const auto preview = service.Bootstrap(user, "review_212_v1", launch_time);
  Check(preview.at("shared_challenge").as_object().at("status") == "ready" &&
            preview.at("session").as_object().at("session_id") == old_id,
        "preview replaced the current training");
  Check(repository.LoadActiveTrainingSession(uid)->state_version == original_snapshot->state_version,
        "preview modified an unrelated training snapshot");
  static_cast<void>(service.RecordActivity(uid, {{"visit_id", "56000000-0000-4000-8000-000000000001"},
                                                 {"sequence", 1},
                                                 {"visible", true},
                                                 {"context", "shared-challenge"}}));
  const auto presence_sql =
      "SELECT context = 'home' AND session_id IS NULL FROM analytics_presence WHERE user_id=" + std::to_string(uid);
  std::unique_ptr<PGresult, decltype(&PQclear)> presence(PQexec(connection.get(), presence_sql.c_str()), PQclear);
  Check(presence && PQresultStatus(presence.get()) == PGRES_TUPLES_OK && PQntuples(presence.get()) == 1 &&
            std::string_view(PQgetvalue(presence.get(), 0, 0)) == "t",
        "a link preview counted as work on the unrelated saved training");
  const auto public_preview = boost::json::serialize(preview.at("shared_challenge"));
  Check(public_preview.find("correct_option_id") == std::string::npos &&
            public_preview.find("explanation") == std::string::npos,
        "challenge preview exposed an answer");
  static_cast<void>(service.Bootstrap(user, "review_212_v1", launch_time));
  const auto after = boost::json::parse(repository.SharedChallengeStatistics(1)).as_object();
  Check(after.at("opened").as_int64() == before.at("opened").as_int64() + 1,
        "a bootstrap retry double-counted a signed Telegram launch");
  Check(service.Bootstrap(user, "review_unknown_v1").at("shared_challenge").as_object().at("status") == "unavailable",
        "unknown links exposed arbitrary questions");
  Reject([&] { static_cast<void>(service.StartSession(uid, {{"challenge_id", "CR-000212"}})); });
  Reject([&] { static_cast<void>(service.StartSession(uid, {{"challenge_id", "review_212_v1"}, {"level", 10}})); });

  const boost::json::object start{{"challenge_id", "review_212_v1"},
                                  {"request_id", "55000000-0000-4000-8000-000000000001"}};
  const auto challenge = service.StartSession(uid, start);
  const auto question = challenge.at("question").as_object();
  const auto challenge_session_id = challenge.at("session_id").as_int64();
  Check(Text(question, "public_id") == "CR-000212" && question.at("shareable").as_bool() &&
            challenge.at("progress").as_object().at("session_goal").as_uint64() == 1 &&
            !challenge.at("tracks_level_progress").as_bool(),
        "the link did not select exactly its non-progression task");
  Check(service.StartSession(uid, start).at("session_id").as_int64() == challenge_session_id,
        "retry created another shared challenge session");
  Reject([&] {
    static_cast<void>(
        service.StartSession(uid, {{"challenge_id", "review_213_v1"}, {"request_id", start.at("request_id")}}));
  });
  const auto snapshot = repository.LoadActiveTrainingSession(uid);
  const auto parsed =
      ParseMiniAppSessionSnapshot(snapshot->state_json, snapshot->session_id, snapshot->state_version, bank);
  Check(parsed.shared_challenge_id == "review_212_v1", "shared challenge origin was not durable");
  auto malformed = boost::json::parse(snapshot->state_json).as_object();
  malformed["shared_challenge_id"] = "review_213_v1";
  Reject([&] {
    static_cast<void>(ParseMiniAppSessionSnapshot(boost::json::serialize(malformed), snapshot->session_id,
                                                  snapshot->state_version, bank));
  });
  MiniAppService restarted(bank, pool, 424242, "cpp_recall_bot");
  const auto restored = restarted.Bootstrap(user).at("session").as_object();
  Check(restored.at("question") == question && Text(restored, "shared_challenge_id") == "review_212_v1",
        "restart lost the challenge or shuffled the presented options");
  static_cast<void>(restarted.StartSession(uid, {{"route_id", "containers_iterators"}}));
  const auto resumed_challenge = restarted.StartSession(uid, {{"challenge_id", "review_212_v1"}});
  Check(resumed_challenge.at("session_id").as_int64() == challenge_session_id &&
            resumed_challenge.at("question") == question,
        "reopening a saved challenge created another session or changed its options");
  const auto before_share = repository.LoadActiveTrainingSession(uid);
  const boost::json::object share_request{{"session_id", challenge_session_id},
                                          {"question_id", question.at("id")},
                                          {"content_revision", question.at("content_revision")}};
  const auto share = restarted.ShareChallenge(uid, share_request);
  Check(Text(share, "url") == "https://t.me/cpp_recall_bot?startapp=review_212_v1" &&
            Text(share, "text").find(bank.Get("review_cache_read_contract").explanation) == std::string::npos &&
            Text(share, "text").find(std::to_string(uid)) == std::string::npos &&
            Text(share, "text").find("correct_option_id") == std::string::npos,
        "share URL or its privacy boundary is incorrect");
  Check(repository.LoadProgressSummary(uid).xp_total == original_xp &&
            repository.LoadActiveTrainingSession(uid)->state_version == before_share->state_version,
        "sharing modified XP or the learning snapshot");
  auto stale_share = share_request;
  stale_share["content_revision"] = std::string(64, '0');
  Reject([&] { static_cast<void>(restarted.ShareChallenge(uid, stale_share)); });
  MiniAppService disabled(bank, pool);
  Reject([&] { static_cast<void>(disabled.ShareChallenge(uid, share_request)); });

  const auto level = repository.GetAdaptiveState(uid);
  const boost::json::object answer{
      {"session_id", challenge_session_id}, {"question_id", question.at("id")}, {"answer", "a"}};
  const auto feedback = restarted.SubmitAnswer(uid, answer);
  Check(restarted.SubmitAnswer(uid, answer) == feedback, "retry double-counted the challenge answer");
  Check(restarted.ShareChallenge(uid, share_request) == share,
        "sharing feedback included a solution or changed the link");
  Check(repository.GetAdaptiveState(uid).level == level.level &&
            repository.GetAdaptiveState(uid).evidence == level.evidence,
        "a pinned public task was used to raise adaptive difficulty");
  const boost::json::object next{{"session_id", challenge_session_id}, {"expected_total_attempts", 1}};
  const auto completed = restarted.NextQuestion(uid, next);
  Check(Text(completed, "status") == "completed" && Text(completed, "shared_challenge_id") == "review_212_v1" &&
            restarted.NextQuestion(uid, next) == completed &&
            restarted.SessionResult(uid, {{"session_id", challenge_session_id}}) == completed,
        "completion recovery lost challenge identity or changed its receipt");
  const auto xp_after = repository.LoadProgressSummary(uid).xp_total;
  const auto saved = restarted.Bootstrap(user).at("saved_sessions").as_array();
  Check(std::ranges::any_of(saved, [&](const auto& item) { return item.as_object().at("id") == old_id; }),
        "starting a linked task lost an unfinished training");
  const auto resumed = restarted.ResumeSession(uid, {{"session_id", old_id}});
  Check(resumed.at("question").as_object().at("id") == old_question.at("id") &&
            resumed.at("question").as_object().at("assisted").as_bool() &&
            resumed.at("question").as_object().at("task") == old_question.at("task"),
        "resuming prior training lost its assistance or option order");
  Check(repository.LoadProgressSummary(uid).xp_total == xp_after, "resuming prior training rolled back XP");
  const auto related = restarted.StartSession(uid, {{"after_challenge_session_id", challenge_session_id}});
  Check(related.at("topics").as_array() == boost::json::array{"containers"} &&
            Text(related.at("question").as_object(), "id") != Text(question, "id") &&
            Text(related, "shared_challenge_id").empty(),
        "follow-up is not ordinary related practice");
  Reject([&] {
    static_cast<void>(restarted.StartSession(uid + 2, {{"after_challenge_session_id", challenge_session_id}}));
  });
  const auto& followup_question = bank.Get(Text(related.at("question").as_object(), "id"));
  const auto& task = followup_question.task;
  const auto followup_answer = std::holds_alternative<ShortAnswerTask>(task)
                                   ? std::get<ShortAnswerTask>(task).accepted_answers.front()
                                   : std::get<SingleChoiceTask>(task).correct_option_id;
  static_cast<void>(restarted.SubmitAnswer(uid, {{"question_id", followup_question.id}, {"answer", followup_answer}}));
  const auto stats = boost::json::parse(repository.SharedChallengeStatistics(1)).as_object();
  Check(stats.at("completed").as_int64() >= 1 && stats.at("answered").as_int64() >= 1 &&
            stats.at("continued").as_int64() >= 1,
        "shared challenge metrics conflate launch, answer, completion and follow-up");

  static_cast<void>(restarted.Bootstrap(beginner));
  const auto beginner_training = restarted.StartSession(beginner.id, {{"route_id", "containers_iterators"}});
  Sql(connection.get(),
      "UPDATE users SET adaptive_level=1, unlocked_level=1 WHERE user_id=" + std::to_string(beginner.id));
  Check(restarted.Bootstrap(beginner, "review_212_v1", launch_time).at("shared_challenge").as_object().at("status") ==
            "locked",
        "a beginner preview ignored the difficulty lock");
  Reject([&] { static_cast<void>(restarted.StartSession(beginner.id, {{"challenge_id", "review_212_v1"}})); });
  Check(restarted.CurrentSession(beginner.id).at("session_id") == beginner_training.at("session_id"),
        "a rejected challenge replaced an existing training");
  Sql(connection.get(), "UPDATE users SET unlocked_level=5 WHERE user_id=" + std::to_string(beginner.id));
  Sql(connection.get(), "INSERT INTO topic_progress(user_id, topic, adaptive_level) VALUES (" +
                            std::to_string(beginner.id) + ", 'containers', 2)");
  Reject([&] { static_cast<void>(restarted.StartSession(beginner.id, {{"challenge_id", "review_212_v1"}})); });
  Sql(connection.get(), "UPDATE topic_progress SET adaptive_level=3 WHERE user_id=" + std::to_string(beginner.id));
  const auto helped = restarted.StartSession(beginner.id, {{"challenge_id", "review_213_v1"}});
  const auto helped_question = helped.at("question").as_object();
  const auto beginner_xp = repository.LoadProgressSummary(beginner.id).xp_total;
  static_cast<void>(restarted.OpenReference(
      beginner.id, {{"question_id", helped_question.at("id")},
                    {"reference_id", helped_question.at("references").as_array().front().as_object().at("id")}}));
  const auto helped_feedback =
      restarted.SubmitAnswer(beginner.id, {{"question_id", helped_question.at("id")}, {"answer", "c"}});
  Check(helped_feedback.at("session").as_object().at("feedback").as_object().at("assisted").as_bool() &&
            repository.LoadProgressSummary(beginner.id).xp_total == beginner_xp,
        "a helped shared answer earned XP or lost its assistance marker");
  auto changed = LoadQuestions("content/questions");
  for (auto& card : changed) {
    if (card.id != "review_cache_read_contract")
      continue;
    card.text += " (test revision)";
    auto content = boost::json::parse(card.content_snapshot_json).as_object();
    content["text"] = card.text;
    card.content_snapshot_json = CanonicalQuestionJson(content);
    card.content_revision = QuestionContentRevision(card.content_snapshot_json);
  }
  const QuestionBank withdrawn(std::move(changed));
  MiniAppService withdrawn_service(withdrawn, pool, 424242, "cpp_recall_bot");
  Check(withdrawn_service
                .Bootstrap({.id = uid + 3, .first_name = {}, .last_name = {}, .username = {}, .language_code = {}},
                           "review_212_v1")
                .at("shared_challenge")
                .as_object()
                .at("status") == "unavailable",
        "an edited unreviewed card remained available through its old public link");
  auto missing = LoadQuestions("content/questions");
  std::erase_if(missing, [](const auto& card) { return card.id == "review_cache_read_contract"; });
  const QuestionBank removed(std::move(missing));
  MiniAppService removed_service(removed, pool, 424242, "cpp_recall_bot");
  Check(removed_service
                .Bootstrap({.id = uid + 4, .first_name = {}, .last_name = {}, .username = {}, .language_code = {}},
                           "review_212_v1")
                .at("shared_challenge")
                .as_object()
                .at("status") == "unavailable",
        "a removed card made bootstrap fail instead of withdrawing its public link");
}
