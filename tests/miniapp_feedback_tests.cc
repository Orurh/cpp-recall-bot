#include "miniapp/miniapp_service.h"
#include "quiz/card_feedback.h"
#include "quiz/question_identity.h"
#include "storage/attempt_repository.h"
#include "storage/question_loader.h"

#include <boost/json.hpp>

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <future>
#include <iostream>
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
