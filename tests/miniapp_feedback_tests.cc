#include "miniapp/miniapp_service.h"
#include "quiz/card_feedback.h"
#include "quiz/question_identity.h"
#include "storage/attempt_repository.h"
#include "storage/question_loader.h"

#include <libpq-fe.h>
#include <boost/json.hpp>

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <future>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>

void CheckBetaStatistics(const char* url);
void CheckAnswerMetrics(const char* url);
void CheckLearningStatistics(const char* url);
void CheckTrainingBalance(const char* url);
void CheckReferenceHelp(const char* url);
void CheckReferenceProvenance(const char* url);
void CheckReviewRecovery(const char* url);
void CheckSharedChallenges(const char* url);
void CheckWrongAnswerHelp(const char* url);
void CheckDailyPracticeService(const char* url);

namespace {

void Require(bool condition, std::string_view message) {
  if (!condition) {
    throw std::runtime_error(std::string(message));
  }
}

template <typename Error, typename Callable>
void RequireThrows(Callable&& callable, std::string_view message) {
  bool threw = false;
  try {
    callable();
  } catch (const Error&) {
    threw = true;
  }
  Require(threw, message);
}

std::string Text(const boost::json::object& object, std::string_view key) {
  return std::string(object.at(key).as_string());
}

void CheckCodingPractice(const char* url) {
  std::unique_ptr<PGconn, decltype(&PQfinish)> connection(PQconnectdb(url), PQfinish);
  Require(connection && PQstatus(connection.get()) == CONNECTION_OK, "coding practice test database unavailable");
  const auto sql = [&](const std::string& query) -> std::string {
    std::unique_ptr<PGresult, decltype(&PQclear)> result(PQexec(connection.get(), query.c_str()), PQclear);
    Require(
        result && (PQresultStatus(result.get()) == PGRES_TUPLES_OK || PQresultStatus(result.get()) == PGRES_COMMAND_OK),
        "coding practice fixture SQL failed");
    return PQntuples(result.get()) == 0 ? "" : PQgetvalue(result.get(), 0, 0);
  };
  const auto uid = 12'000'000'000 + std::chrono::steady_clock::now().time_since_epoch().count() % 1'000'000'000;
  const auto filter = "user_id=" + std::to_string(uid);
  auto pool = std::make_shared<DatabaseConnectionPool>(url, 3);
  AttemptRepository repository(pool);
  QuestionBank bank(LoadQuestions("content/questions"));
  MiniAppService service(bank, pool);
  const TelegramMiniAppUser user{.id = uid, .first_name = {}, .last_name = {}, .username = {}, .language_code = {}};
  const auto metadata = [&] {
    return service.Bootstrap(user).at("coding_practice").as_object();
  };
  auto practice = metadata();
  Require(Text(practice, "id") == "take_prefix" && practice.at("revision").as_int64() == 1 &&
              practice.at("level").as_int64() == 3 &&
              Text(practice, "skill_id") == "containers.iterator_invalidation" &&
              Text(practice, "topic") == "containers",
          "coding practice metadata contract changed");
  Require(!practice.at("available").as_bool() && Text(practice, "reason").find("std::vector") != std::string::npos,
          "a novice received an independent function without prerequisite evidence");
  sql("INSERT INTO user_skill_state(user_id,skill_id,status) VALUES (" + std::to_string(uid) +
      ",'containers.vector','encountered')");
  sql("INSERT INTO skill_progress(user_id,skill_id,question_id) VALUES (" + std::to_string(uid) +
      ",'containers.vector','vector_reserve_does_not_change_size')");
  Require(!metadata().at("available").as_bool(), "mere familiarity unlocked independent coding");
  sql("UPDATE user_skill_state SET status='demonstrated' WHERE " + filter);
  practice = metadata();
  Require(practice.at("available").as_bool() && Text(practice, "reason").empty(),
          "eligible L3 learner could not open independent coding");
  sql("INSERT INTO topic_progress(user_id,topic,adaptive_level) VALUES (" + std::to_string(uid) + ",'containers',3)");
  sql("UPDATE users SET adaptive_level=1,unlocked_level=1 WHERE " + filter);
  practice = metadata();
  Require(!practice.at("available").as_bool() && Text(practice, "reason").find("L3") != std::string::npos,
          "prerequisite evidence bypassed the unlocked difficulty");
  sql("UPDATE users SET adaptive_level=5,unlocked_level=5 WHERE " + filter);
  sql("UPDATE topic_progress SET adaptive_level=2 WHERE " + filter);
  Require(!metadata().at("available").as_bool(), "global level bypassed the containers difficulty");
  sql("UPDATE topic_progress SET adaptive_level=3 WHERE " + filter);
  sql("UPDATE user_skill_state SET status='needs_recovery' WHERE " + filter);
  Require(!metadata().at("available").as_bool(), "a prerequisite awaiting recovery unlocked independent coding");
  sql("UPDATE user_skill_state SET status='retained' WHERE " + filter);
  Require(metadata().at("available").as_bool(), "retained prerequisite did not unlock independent coding");

  const auto session = service.StartSession(uid, {{"recommended", true}});
  const auto saved_before = repository.LoadActiveTrainingSession(uid);
  const auto progress_before = repository.LoadProgressSummary(uid);
  Require(saved_before.has_value(), "coding activity fixture did not preserve a training");
  // A restarted service must not restore/attribute the saved card while observing local code work.
  MiniAppService restarted(bank, pool);
  boost::json::object pulse{
      {"visit_id", "cf010000-0000-4000-8000-000000000001"}, {"sequence", 1}, {"visible", true}, {"context", "coding"}};
  Require(restarted.RecordActivity(uid, pulse).at("accepted").as_bool(), "coding activity was rejected");
  Require(sql("SELECT context || ':' || COALESCE(session_id::TEXT,'') || ':' || question_id || ':' || practice_kind "
              "FROM analytics_presence WHERE " +
              filter) == "home:::",
          "coding activity was attributed to an unrelated active card");
  sql("UPDATE analytics_presence SET last_seen_at=now()-interval '4 seconds' WHERE " + filter);
  pulse["sequence"] = 2;
  static_cast<void>(restarted.RecordActivity(uid, pulse));
  Require(sql("SELECT active_ms::TEXT || ':' || context || ':' || COALESCE(session_id::TEXT,'') || ':' || question_id "
              "FROM activity_samples WHERE " +
              filter + " AND sequence=2") == "0:home::",
          "local ungraded work became measured time on an unrelated card");
  Require(sql("SELECT count(*) FROM learning_events WHERE " + filter + " AND event_name='session_resumed'") == "0",
          "coding activity falsely recorded a training resume");
  const auto saved_after = repository.LoadActiveTrainingSession(uid);
  const auto progress_after = repository.LoadProgressSummary(uid);
  Require(saved_after && saved_after->session_id == session.at("session_id").as_int64() &&
              saved_after->state_version == saved_before->state_version &&
              saved_after->state_json == saved_before->state_json,
          "local coding activity changed the saved training");
  Require(progress_after.xp_total == progress_before.xp_total &&
              progress_after.answers.attempts == progress_before.answers.attempts &&
              progress_after.completed_count == progress_before.completed_count,
          "local coding activity changed scored learning progress");
}

void CheckBugHunter(const char* url) {
  auto pool = std::make_shared<DatabaseConnectionPool>(url, 3);
  AttemptRepository repository(pool);
  const auto cards = LoadQuestions("content/questions");
  const auto found = std::ranges::find(cards, "vector_reserve_bug_line", &Question::id);
  Require(found != cards.end(), "missing real Bug Hunter card");
  std::vector<Question> fixtures{*found};
  for (const auto& other : cards) {
    if (fixtures.size() == 5)
      break;
    if (other.level == found->level && other.topic != found->topic)
      fixtures.push_back(other);
  }
  Require(fixtures.size() == 5, "missing Bug Hunter level fixtures");
  QuestionBank bank(fixtures);
  std::unique_ptr<PGconn, decltype(&PQfinish)> db(PQconnectdb(url), PQfinish);
  Require(db && PQstatus(db.get()) == CONNECTION_OK, "Bug Hunter test DB unavailable");
  const auto base = -22'000'000'000 - std::chrono::steady_clock::now().time_since_epoch().count() % 1'000'000'000;
  for (int scenario = 0; scenario < 3; ++scenario) {
    const auto uid = base - scenario;
    repository.EnsureUser(uid);
    const auto sql = "UPDATE users SET adaptive_level=3, unlocked_level=3 WHERE user_id=" + std::to_string(uid);
    std::unique_ptr<PGresult, decltype(&PQclear)> updated(PQexec(db.get(), sql.c_str()), PQclear);
    Require(updated && PQresultStatus(updated.get()) == PGRES_COMMAND_OK, "cannot set Bug Hunter level");
    MiniAppService service(bank, pool);
    const auto first = service.StartSession(
        uid,
        {{"modes", boost::json::array{"review_code"}}, {"topics", boost::json::array{"containers"}}, {"level", 3}});
    const auto sid = first.at("session_id");
    const auto& question = first.at("question").as_object();
    Require(Text(question, "id") == found->id && Text(question, "kind") == "debugging" &&
                question.at("references").as_array().empty() && !question.contains("explanation") &&
                !question.at("task").as_object().contains("correct_option_id"),
            "Bug Hunter payload disclosed the key or a ready-answer reference");
    const auto initial = repository.LoadActiveTrainingSession(uid);
    RequireThrows<std::invalid_argument>(
        [&] { static_cast<void>(service.SubmitAnswer(uid, {{"question_id", found->id}, {"answer", "line_06"}})); },
        "forged line ID was accepted");
    Require(repository.LoadActiveTrainingSession(uid)->state_version == initial->state_version,
            "forged line changed saved attempts");
    const auto answer = [&](MiniAppService& target, std::string_view line) {
      return target.SubmitAnswer(uid, {{"question_id", found->id}, {"session_id", sid}, {"answer", line}})
          .at("session")
          .as_object()
          .at("feedback")
          .as_object();
    };
    if (scenario != 1) {
      const auto wrong = answer(service, "line_5");
      Require(!wrong.at("correct").as_bool() && !wrong.at("completed").as_bool() &&
                  wrong.at("xp_gained").as_int64() == 0 && !wrong.contains("explanation") &&
                  !wrong.contains("correct_answer"),
              "wrong line completed the card or disclosed its solution");
      const auto snapshot = repository.LoadActiveTrainingSession(uid);
      MiniAppService resumed(bank, pool);
      const auto restored = resumed.CurrentSession(uid);
      Require(Text(restored, "phase") == "feedback" &&
                  repository.LoadActiveTrainingSession(uid)->state_version == snapshot->state_version,
              "resuming Bug Hunter resubmitted the answer");
      if (scenario == 2) {
        const auto revealed = resumed.OpenTheory(uid, {{"session_id", sid},
                                                       {"question_id", found->id},
                                                       {"content_revision", found->content_revision},
                                                       {"kind", "answer"}});
        Require(revealed.at("session").as_object().at("feedback").as_object().at("answer_revealed").as_bool(),
                "Bug Hunter stock answer help did not reveal on request");
      }
      const auto retry = resumed.NextQuestion(uid, {{"session_id", sid}, {"expected_total_attempts", 1}});
      Require(Text(retry.at("question").as_object(), "id") == found->id, "wrong line disappeared from repair");
    }
    MiniAppService restored(bank, pool);
    static_cast<void>(restored.CurrentSession(uid));
    const auto correct = answer(restored, "line_6");
    Require(correct.at("correct").as_bool() && correct.at("completed").as_bool() &&
                correct.at("attempts").as_uint64() == (scenario == 1 ? 1U : 2U) &&
                correct.at("xp_gained").as_int64() == (scenario == 1   ? 12
                                                       : scenario == 0 ? 6
                                                                       : 0),
            "Bug Hunter changed ordinary attempts or XP");
    const auto& mastery = correct.at("skill_mastery").as_object();
    Require(mastery.at("successful_families").as_int64() == 1 &&
                mastery.at("clean_families").as_int64() == (scenario == 1 ? 1 : 0) &&
                mastery.at("application_families").as_int64() == 0 && Text(mastery, "current") == "encountered",
            "Bug Hunter recognition became independent application/mastery");
  }
}

}  // namespace

int main() {
  const char* url = std::getenv("TEST_DATABASE_URL");
  if (url == nullptr || *url == '\0') {
    std::cout << "miniapp_feedback_tests skipped: TEST_DATABASE_URL is not set\n";
    return 77;
  }
  try {
    CheckBetaStatistics(url);
    CheckLearningStatistics(url);
    CheckAnswerMetrics(url);
    CheckTrainingBalance(url);
    CheckReferenceHelp(url);
    CheckReferenceProvenance(url);
    CheckReviewRecovery(url);
    CheckSharedChallenges(url);
    CheckWrongAnswerHelp(url);
    CheckDailyPracticeService(url);
    CheckCodingPractice(url);
    CheckBugHunter(url);
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    const auto user_id = -1 - static_cast<std::int64_t>(stamp % 1'000'000'000);
    constexpr std::int64_t admin_id = 424242;
    auto pool = std::make_shared<DatabaseConnectionPool>(url, 3);
    QuestionBank bank(LoadQuestions("content/questions"));
    MiniAppService service(bank, pool, admin_id);
    AttemptRepository repository(pool);

    Require(!service.IsFeedbackAdmin(user_id) && service.IsFeedbackAdmin(admin_id), "feedback owner check failed");
    RequireThrows<FeedbackAccessDenied>([&] { static_cast<void>(service.ListCardFeedback(user_id, {})); },
                                        "a regular user accessed the editorial queue");
    RequireThrows<FeedbackAccessDenied>([&] { static_cast<void>(service.ReviewCardFeedback(user_id, {})); },
                                        "a regular user could review feedback");
    const auto session = service.StartSession(user_id, {{"recommended", true}});
    const auto question = session.at("question").as_object();
    Require(question.contains("public_id") && question.contains("content_revision"), "card identity was not exposed");
    const auto question_id = Text(question, "id");
    const auto& original = bank.Get(question_id);
    const auto response_json = boost::json::serialize(question);
    Require(response_json.find("correct_option_id") == std::string::npos &&
                response_json.find("accepted_answers") == std::string::npos &&
                response_json.find("content_snapshot") == std::string::npos,
            "a public card leaked its expected answer or snapshot");
    boost::json::object request{
        {"request_id", "10000000-0000-4000-8000-000000000001"},
        {"question_id", question_id},
        {"content_revision", question.at("content_revision")},
        {"phase", "question"},
        {"category", "unclear_wording"},
        {"comment", "Непонятное условие <script>alert(1)</script>"},
    };
    std::promise<void> start;
    auto ready = start.get_future().share();
    auto first = std::async(std::launch::async, [&] {
      ready.wait();
      return service.SubmitCardFeedback(user_id, request);
    });
    auto second = std::async(std::launch::async, [&] {
      ready.wait();
      return service.SubmitCardFeedback(user_id, request);
    });
    start.set_value();
    const auto receipt = first.get();
    Require(receipt.at("feedback_id") == second.get().at("feedback_id"),
            "concurrent retries created duplicate feedback");
    Require(!receipt.contains("content") && !receipt.contains("comment"), "feedback receipt leaked editorial data");
    auto changed_request = request;
    changed_request["comment"] = "Другой текст";
    RequireThrows<std::logic_error>([&] { static_cast<void>(service.SubmitCardFeedback(user_id, changed_request)); },
                                    "a reused idempotency key accepted another comment");
    auto invalid = request;
    invalid["request_id"] = "10000000-0000-4000-8000-000000000002";
    invalid["content_revision"] = std::string(64, '0');
    RequireThrows<std::logic_error>([&] { static_cast<void>(service.SubmitCardFeedback(user_id, invalid)); },
                                    "an unpublished card revision was accepted");
    invalid["content_revision"] = original.content_revision;
    invalid["phase"] = "feedback";
    RequireThrows<std::logic_error>([&] { static_cast<void>(service.SubmitCardFeedback(user_id, invalid)); },
                                    "a feedback report claimed the wrong presentation phase");
    invalid["phase"] = "question";
    invalid["comment"] = "   \n";
    RequireThrows<std::invalid_argument>([&] { static_cast<void>(service.SubmitCardFeedback(user_id, invalid)); },
                                         "a whitespace-only report was accepted");

    auto queue = service.ListCardFeedback(admin_id, {{"status", "new"}}).at("items").as_array();
    Require(!queue.empty(), "the owner could not see a new report");
    const auto item = queue.front().as_object();
    Require(item.at("feedback_id") == receipt.at("feedback_id") && Text(item, "public_id") == original.public_id,
            "feedback was assigned to the wrong card");
    Require(QuestionContentRevision(CanonicalQuestionJson(item.at("content"))) == original.content_revision,
            "the feedback snapshot does not match the displayed content revision");
    boost::json::object review{
        {"feedback_id", receipt.at("feedback_id")},
        {"version", item.at("version")},
        {"status", "resolved"},
        {"decision", ""},
        {"source", ""},
    };
    RequireThrows<std::invalid_argument>([&] { static_cast<void>(service.ReviewCardFeedback(admin_id, review)); },
                                         "feedback was closed without a decision and source");
    review["status"] = "in_review";
    const auto taken = service.ReviewCardFeedback(admin_id, review);
    Require(Text(taken, "status") == "in_review", "feedback could not be taken for review");
    RequireThrows<std::logic_error>([&] { static_cast<void>(service.ReviewCardFeedback(admin_id, review)); },
                                    "a stale editor overwrote a newer review");
    review["version"] = taken.at("version");
    review["status"] = "resolved";
    review["decision"] = "Формулировка уточнена после проверки.";
    review["source"] = "https://eel.is/c++draft/intro";
    static_cast<void>(service.ReviewCardFeedback(admin_id, review));

    // A changed bank must retain the original revision, including its original key and explanation.
    auto revised = original;
    auto revised_json = boost::json::parse(original.content_snapshot_json).as_object();
    revised_json["explanation"] = "Updated explanation for archive test";
    revised.content_snapshot_json = CanonicalQuestionJson(revised_json);
    revised.content_revision = QuestionContentRevision(revised.content_snapshot_json);
    repository.ArchiveQuestionContent({&revised});
    const auto archived = repository.LoadQuestionContent(question_id, original.content_revision);
    Require(archived && QuestionContentRevision(CanonicalQuestionJson(boost::json::parse(*archived))) ==
                            original.content_revision,
            "archiving a new revision destroyed the old content");
    MiniAppService restarted(bank, pool, admin_id);
    static_cast<void>(restarted.StopSession(user_id));
    Require(restarted.SubmitCardFeedback(user_id, request).at("feedback_id") == receipt.at("feedback_id"),
            "a lost response could not be retried after restart and session stop");

    const auto summary = repository.LoadProgressSummary(user_id);
    Require(summary.completed_count == 0 && summary.xp_total == 0, "reporting feedback changed learning progress");

    const auto feedback_user = user_id - 1;
    const auto feedback_ids = bank.QuestionIds();
    const auto introductory = std::ranges::find_if(feedback_ids, [&bank](const std::string& id) {
      const auto& card = bank.Get(id);
      return card.skill_id == "core.storage_duration" && card.level == 1 &&
             std::holds_alternative<SingleChoiceTask>(card.task);
    });
    Require(introductory != feedback_ids.end(), "feedback fixture needs an introductory single-choice card");
    QuestionBank feedback_bank({bank.Get(*introductory)});
    MiniAppService feedback_service(feedback_bank, pool, admin_id);
    const auto feedback_session = feedback_service.StartSession(feedback_user, {{"recommended", true}});
    const auto feedback_question = feedback_session.at("question").as_object();
    const auto& feedback_card = bank.Get(Text(feedback_question, "id"));
    const auto* choice = std::get_if<SingleChoiceTask>(&feedback_card.task);
    Require(choice != nullptr, "the recommended introductory card must be a single-choice task for this test");
    const auto answered =
        service.SubmitAnswer(feedback_user, {{"question_id", feedback_card.id}, {"answer", choice->correct_option_id}});
    const auto answer_feedback = answered.at("session").as_object().at("feedback").as_object();
    Require(Text(answer_feedback, "public_id") == feedback_card.public_id &&
                Text(answer_feedback, "content_revision") == feedback_card.content_revision,
            "the answer explanation lost the card identity");
    auto feedback_request = request;
    feedback_request["question_id"] = feedback_card.id;
    feedback_request["content_revision"] = feedback_card.content_revision;
    feedback_request["phase"] = "feedback";
    static_cast<void>(service.SubmitCardFeedback(feedback_user, feedback_request));
    auto saved_report = *repository.FindCardFeedback(feedback_user, Text(feedback_request, "request_id"));
    Require(saved_report.submission.phase == "feedback", "a report from the explanation saved the wrong phase");

    // Exercise database-level deduplication across repositories without the service's per-user mutex.
    auto concurrent_draft = saved_report.submission;
    concurrent_draft.request_id = "10000000-0000-4000-8000-000000000002";
    AttemptRepository other_repository(pool);
    std::promise<void> storage_start;
    auto storage_ready = storage_start.get_future().share();
    auto storage_first = std::async(std::launch::async, [&] {
      storage_ready.wait();
      return repository.SaveCardFeedback(concurrent_draft);
    });
    auto storage_second = std::async(std::launch::async, [&] {
      storage_ready.wait();
      return other_repository.SaveCardFeedback(concurrent_draft);
    });
    storage_start.set_value();
    Require(storage_first.get().id == storage_second.get().id, "database-level concurrent retries created duplicates");
    for (int i = 3; i <= 20; ++i) {
      auto draft = saved_report.submission;
      draft.request_id = "10000000-0000-4000-8000-0000000000" + std::to_string(i + 10);
      static_cast<void>(repository.SaveCardFeedback(draft));
    }
    auto excessive = saved_report.submission;
    excessive.request_id = "10000000-0000-4000-8000-000000000099";
    RequireThrows<FeedbackRateLimitError>([&] { static_cast<void>(repository.SaveCardFeedback(excessive)); },
                                          "feedback rate limit was not enforced");
    Require(repository.SaveCardFeedback(saved_report.submission).id == saved_report.id,
            "feedback rate limit prevented retrying an already accepted report");
    static_cast<void>(service.StopSession(feedback_user));
    MiniAppService disabled_admin(bank, pool);
    RequireThrows<FeedbackAccessDenied>([&] { static_cast<void>(disabled_admin.ListCardFeedback(admin_id, {})); },
                                        "the editorial queue was open with no owner configured");
    std::cout << "miniapp_feedback_tests passed\n";
  } catch (const std::exception& error) {
    std::cerr << "miniapp_feedback_tests failed: " << error.what() << '\n';
    return 1;
  }
}
