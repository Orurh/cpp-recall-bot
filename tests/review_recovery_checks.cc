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
#include <variant>

namespace {
void Check(bool condition, const char* message) {
  if (!condition) {
    throw std::runtime_error(message);
  }
}

void Sql(PGconn* db, const std::string& sql) {
  std::unique_ptr<PGresult, decltype(&PQclear)> result(PQexec(db, sql.c_str()), PQclear);
  Check(result && PQresultStatus(result.get()) == PGRES_COMMAND_OK, "review recovery SQL fixture failed");
}

void SeedRetained(AttemptRepository& repository, PGconn* db, std::int64_t uid, const Question& question, bool due) {
  const auto session = repository.StartSession(uid, "integration-review-seed");
  const auto other_question = question.id + "-seed";
  const auto other_family = question.question_family_id + "-seed";
  SubmissionRecord evidence{
      .user_id = uid,
      .session_id = session,
      .question_id = other_question,
      .topic = question.topic,
      .skill_id = question.skill_id,
      .question_family_id = other_family,
      .evidence_type = QuestionEvidenceType::kRecognition,
      .answer = "synthetic seed",
      .correct = true,
      .completed = true,
      .attempt_number = 1,
  };
  static_cast<void>(repository.RecordSubmission(evidence));
  evidence.question_id = question.id;
  evidence.question_family_id = question.question_family_id;
  evidence.evidence_type = QuestionEvidenceType::kApplication;
  static_cast<void>(repository.RecordSubmission(evidence));
  repository.FinishSession(session, "completed");
  const auto owner = std::to_string(uid);
  // Synthetic old learning, not a claim about a real person's retained knowledge.
  Sql(db,
      "UPDATE user_skill_state SET status = 'retained', demonstrated_at = now() - INTERVAL '2 days', "
      "retained_at = now() - INTERVAL '1 day' WHERE user_id = " +
          owner);
  Sql(db, "UPDATE question_progress SET review_stage = 5, consecutive_correct = 5, next_review_at = now() " +
              std::string(due ? "-" : "+") + " INTERVAL '1 day' WHERE user_id = " + owner);
}

TelegramMiniAppUser User(std::int64_t uid) {
  return {.id = uid, .first_name = {}, .last_name = {}, .username = {}, .language_code = {}};
}
}  // namespace

void CheckReviewRecovery(const char* url) {
  const auto questions = LoadQuestions("content/questions");
  auto pool = std::make_shared<DatabaseConnectionPool>(url, 3);
  AttemptRepository repository(pool);
  std::unique_ptr<PGconn, decltype(&PQfinish)> db(PQconnectdb(url), PQfinish);
  Check(db && PQstatus(db.get()) == CONNECTION_OK, "review recovery database unavailable");
  const auto base = -4'000'000'000 - std::chrono::steady_clock::now().time_since_epoch().count() % 1'000'000'000;

  for (std::size_t kind = 0; kind < 4; ++kind) {
    const auto card = std::ranges::find_if(
        questions, [kind](const Question& question) { return question.task.index() == kind && question.level <= 3; });
    Check(card != questions.end(), "review recovery task fixture missing");
    const auto uid = base - static_cast<std::int64_t>(kind);
    QuestionBank bank({*card});
    SeedRetained(repository, db.get(), uid, *card, true);
    const auto before = repository.LoadProgressSummary(uid);
    MiniAppService service(bank, pool);
    auto current = service.StartSession(uid, {{"recommended", true}});
    const auto saved = repository.LoadActiveTrainingSession(uid);
    Check(saved.has_value(), "review recovery snapshot missing");
    bool conflict_rejected = false;
    try {
      static_cast<void>(repository.UpdateActiveTrainingSession(uid, saved->session_id, saved->state_version + 1,
                                                               saved->content_version, saved->state_json,
                                                               ReviewFailureContext{card->id, card->skill_id}));
    } catch (const std::logic_error&) {
      conflict_rejected = true;
    }
    Check(conflict_rejected &&
              repository.LoadProgressSummary(uid).skills.front().mastery_status == SkillMasteryStatus::kRetained &&
              repository.LoadReviewStates(uid).at(card->id).stage == 5 &&
              repository.LoadActiveTrainingSession(uid)->state_version == saved->state_version,
          "a rejected partial failure changed mastery or review progress");

    if (kind < 3) {
      boost::json::value wrong;
      if (const auto* single = std::get_if<SingleChoiceTask>(&card->task)) {
        wrong = std::ranges::find_if(single->options, [&](const auto& option) {
                  return option.id != single->correct_option_id;
                })->id;
      } else if (const auto* multiple = std::get_if<MultipleChoiceTask>(&card->task)) {
        boost::json::array answers;
        // Toggle one option relative to the correct set; keep the request nonempty.
        for (const auto& option : multiple->options) {
          const bool correct =
              std::ranges::find(multiple->correct_option_ids, option.id) != multiple->correct_option_ids.end();
          if (correct != (option.id == multiple->options.front().id)) {
            answers.emplace_back(option.id);
          }
        }
        if (answers.empty()) {
          answers.emplace_back(multiple->options.back().id);
        }
        wrong = std::move(answers);
      } else {
        wrong = "intentionally incorrect review answer";
      }
      const boost::json::object request{{"question_id", card->id}, {"answer", wrong}};
      const auto response = service.SubmitAnswer(uid, request);
      const auto& feedback = response.at("session").as_object().at("feedback").as_object();
      Check(!feedback.at("completed").as_bool() &&
                feedback.at("skill_mastery").as_object().at("current").as_string() == "needs_recovery",
            "unfinished wrong answer did not report recovery");
      static_cast<void>(service.SubmitAnswer(uid, request));
    } else {
      const auto& pairs = std::get<MatchingTask>(card->task).pairs;
      Check(pairs.size() >= 2, "matching recovery fixture needs two pairs");
      const auto pick = [&](std::string_view side, std::string_view text) {
        const auto& board = current.at("question").as_object().at("task").as_object().at("board").as_object();
        const auto& column = board.at(side).as_array();
        const auto item = std::ranges::find_if(
            column, [&](const auto& value) { return value.as_object().at("text").as_string() == text; });
        Check(item != column.end(), "matching recovery item missing");
        const boost::json::object request{{"question_id", card->id},
                                          {"side", side},
                                          {"position", item->as_object().at("position")},
                                          {"revision", board.at("revision")}};
        const auto response = service.SelectMatching(uid, request);
        current = response.at("session").as_object();
        static_cast<void>(service.SelectMatching(uid, request));  // Same revision must not apply the mistake twice.
      };
      pick("left", pairs[0].left);
      pick("right", pairs[1].right);
    }

    MiniAppService reopened(bank, pool);
    static_cast<void>(reopened.Bootstrap(User(uid)));
    const auto restored = repository.LoadActiveTrainingSession(uid);
    const auto after = repository.LoadProgressSummary(uid);
    Check(restored && restored->session_id == saved->session_id && restored->state_version > saved->state_version &&
              after.skills.front().mastery_status == SkillMasteryStatus::kNeedsRecovery &&
              after.skills.front().clean_family_count == before.skills.front().clean_family_count &&
              after.xp_total == before.xp_total && after.completed_count == before.completed_count,
          "reopening lost a failure or rewrote evidence, completed count or XP");
    const auto state = repository.LoadReviewStates(uid).at(card->id);
    Check(state.due && state.stage == 1, "an unfinished failure removed its due cycle or retained a long interval");
    Check(after.answers.attempts == before.answers.attempts + (kind == 3 ? 0 : 1),
          "matching clicks or duplicate answers polluted answer statistics");

    // A stale partial matching write must not change state or mastery.
    bool stale_rejected = false;
    try {
      static_cast<void>(repository.UpdateActiveTrainingSession(uid, saved->session_id, saved->state_version,
                                                               saved->content_version, saved->state_json,
                                                               ReviewFailureContext{card->id, card->skill_id}));
    } catch (const std::logic_error&) {
      stale_rejected = true;
    }
    Check(stale_rejected && repository.LoadActiveTrainingSession(uid)->state_version == restored->state_version,
          "a stale failure snapshot was accepted");
    static_cast<void>(reopened.StopSession(uid));
  }

  // Voluntary early practice must not declare a retained skill lost.
  const auto& card = *std::ranges::find_if(questions, [](const auto& question) {
    return question.level <= 3 && std::holds_alternative<SingleChoiceTask>(question.task);
  });
  const auto uid = base - 10;
  SeedRetained(repository, db.get(), uid, card, false);
  const auto session = repository.StartSession(uid, "integration-early-review");
  const auto failed = repository.RecordSubmission({.user_id = uid,
                                                   .session_id = session,
                                                   .question_id = card.id,
                                                   .topic = card.topic,
                                                   .skill_id = card.skill_id,
                                                   .question_family_id = card.question_family_id,
                                                   .answer = "incorrect",
                                                   .attempt_number = 1});
  Check(!failed.skill_mastery &&
            repository.LoadProgressSummary(uid).skills.front().mastery_status == SkillMasteryStatus::kRetained,
        "an early practice mistake was treated as a failed due review");
  repository.FinishSession(session, "miniapp_stopped");
}
