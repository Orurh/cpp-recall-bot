#include "learning/reference_catalog.h"
#include "learning/theory_catalog.h"
#include "miniapp/miniapp_service.h"
#include "miniapp/session_snapshot.h"
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
  Check(rejected, "unavailable or stale answer help was accepted");
}
std::string Text(const boost::json::object& object, std::string_view key) {
  return std::string(object.at(key).as_string());
}
void Sql(PGconn* db, const std::string& sql) {
  std::unique_ptr<PGresult, decltype(&PQclear)> result(PQexec(db, sql.c_str()), PQclear);
  Check(result && PQresultStatus(result.get()) == PGRES_COMMAND_OK, "cannot prepare answer-help user");
}
void Identify(Question& card, const boost::json::object& content) {
  card.content_snapshot_json = CanonicalQuestionJson(content);
  card.content_revision = QuestionContentRevision(card.content_snapshot_json);
}

void CheckMechanismHelp(const std::vector<Question>& cards, const std::shared_ptr<DatabaseConnectionPool>& pool,
                        AttemptRepository& repository, PGconn* db, std::int64_t uid) {
  for (int scenario = 0; scenario < 6; ++scenario, --uid) {
    const std::string target = scenario == 3   ? "copy_constructor_const_ref_non_const_source"
                               : scenario == 5 ? "static_object_surrounds_main"
                                               : "construct_on_first_use_cross_tu";
    const auto found = std::ranges::find(cards, target, &Question::id);
    Check(found != cards.end(), "missing mechanism-help real fixture");
    const auto card = *found;
    std::vector<Question> fixtures{card};
    for (const auto& other : cards) {
      if (fixtures.size() == 5)
        break;
      if (other.level == card.level && other.topic != card.topic)
        fixtures.push_back(other);
    }
    Check(fixtures.size() == 5, "missing selectable level fixtures for mechanism help");
    QuestionBank bank(fixtures);
    repository.EnsureUser(uid);
    const auto level = std::to_string(card.level);
    Sql(db, "UPDATE users SET adaptive_level = " + level + ", unlocked_level = " + level +
                " WHERE user_id = " + std::to_string(uid));
    MiniAppService service(bank, pool);
    const auto initial = service.StartSession(
        uid,
        {{"modes", boost::json::array{"code"}}, {"topics", boost::json::array{card.topic}}, {"level", card.level}});
    Check(Text(initial.at("question").as_object(), "id") == card.id, "wrong mechanism fixture selected");
    const auto session_id = initial.at("session_id");
    std::string wrong_answer = "not-a-sequence";
    std::string correct_answer;
    if (const auto* single = std::get_if<SingleChoiceTask>(&card.task)) {
      wrong_answer = std::ranges::find_if(single->options, [&](const auto& option) {
                       return option.id != single->correct_option_id;
                     })->id;
      correct_answer = single->correct_option_id;
    } else {
      correct_answer = std::get<ShortAnswerTask>(card.task).accepted_answers.front();
    }
    const auto receipt =
        service.SubmitAnswer(uid, {{"question_id", card.id}, {"session_id", session_id}, {"answer", wrong_answer}});
    const auto& feedback = receipt.at("session").as_object().at("feedback").as_object();
    Check(!feedback.at("correct").as_bool() && !feedback.at("completed").as_bool() &&
              feedback.at("theory_available").as_bool() && !feedback.contains("correct_answer") &&
              !feedback.contains("explanation") && feedback.at("answer_available").as_bool() == (scenario != 3),
          "wrong feedback hid mechanism help or disclosed the answer");
    const auto before = repository.LoadActiveTrainingSession(uid);
    auto saved = boost::json::parse(before->state_json).as_object();
    auto& saved_feedback = saved.at("last_feedback").as_object();
    if (scenario == 1 || scenario == 2) {
      saved_feedback.erase("content_revision");
      saved_feedback.erase("answer_available");
      saved_feedback.erase("explanation_available");
      if (scenario == 1) {
        saved_feedback.erase("skill_id");
        saved_feedback.erase("question_family_id");
      }
    } else if (scenario == 3) {
      // A pre-patch wrong result had false availability and no mechanism metadata.
      saved_feedback["theory_available"] = false;
      saved_feedback.erase("skill_id");
      saved_feedback.erase("question_family_id");
    } else if (scenario == 4) {
      saved["current_content_revision"] = std::string(64, '0');
      saved_feedback["content_revision"] = std::string(64, '0');
    }
    if (scenario == 0) {
      // The pinned archive must win over both saved hints and today's same-ID family.
      saved_feedback["skill_id"] = "special_members.copy_construction";
      saved_feedback["question_family_id"] = "special_members.copy_construction.other";
      auto revised = card;
      auto content = boost::json::parse(card.content_snapshot_json).as_object();
      revised.question_family_id = "core.storage_duration.static_lifetime";
      content["question_family_id"] = revised.question_family_id;
      Identify(revised, content);
      fixtures.front() = revised;
    }
    static_cast<void>(repository.UpdateActiveTrainingSession(uid, before->session_id, before->state_version,
                                                             before->content_version, boost::json::serialize(saved)));
    QuestionBank later_bank(fixtures);
    MiniAppService restored(later_bank, pool);
    const auto current = restored.CurrentSession(uid);
    Check(current.at("feedback").as_object().at("theory_available").as_bool() == (scenario != 4),
          "restored feedback availability disagreed with its archived mechanism");
    const boost::json::object request{{"question_id", card.id}, {"session_id", session_id}};
    if (scenario == 4) {
      const auto unavailable = repository.LoadActiveTrainingSession(uid);
      Reject([&] { static_cast<void>(restored.OpenTheory(uid, request)); });
      Check(repository.LoadActiveTrainingSession(uid)->state_version == unavailable->state_version &&
                boost::json::parse(unavailable->state_json)
                    .as_object()
                    .at("theory_viewed_question_ids")
                    .as_array()
                    .empty(),
            "missing archive wrote assistance or substituted today's help");
      static_cast<void>(restored.StopSession(uid));
      continue;
    }
    if (scenario == 1 || scenario == 2 || scenario == 3) {
      const auto unchanged = repository.LoadActiveTrainingSession(uid);
      Reject([&] { static_cast<void>(restored.OpenTheory(uid, {{"question_id", card.id}, {"kind", "answer"}})); });
      Check(repository.LoadActiveTrainingSession(uid)->state_version == unchanged->state_version,
            "unsupported answer disclosure wrote assistance");
    }
    const auto opened = restored.OpenTheory(uid, request);
    const std::string expected = scenario == 3                    ? "copy_construction.object_lifecycle"
                                 : scenario == 1 || scenario == 5 ? "storage_duration.basics"
                                                                  : "storage_duration.initialization_order";
    Check(Text(opened.at("theory").as_object(), "id") == expected &&
              !opened.at("session").as_object().at("feedback").as_object().contains("correct_answer") &&
              !opened.at("session").as_object().at("feedback").as_object().contains("explanation"),
          "principle selected the wrong mechanism or disclosed the exact answer");
    const auto persisted = repository.LoadActiveTrainingSession(uid);
    const auto state = boost::json::parse(persisted->state_json).as_object();
    Check(state.at("theory_viewed_question_ids").as_array().size() == 1 &&
              state.at("last_feedback").as_object().at("theory_viewed").as_bool() &&
              repository.LoadProgressSummary(uid).completed_count == 0 &&
              repository.LoadProgressSummary(uid).xp_total == 0 && repository.LoadCleanQuestionFamilies(uid).empty(),
          "principle help was not persisted or counted as progress");
    Check(restored.OpenTheory(uid, request) == opened &&
              repository.LoadActiveTrainingSession(uid)->state_version == persisted->state_version,
          "mechanism help replay duplicated assistance");
    MiniAppService restarted(bank, pool);
    Check(Text(restarted.OpenTheory(uid, request).at("theory").as_object(), "id") == expected,
          "restart changed the saved mechanism");
    const auto repair = restarted.NextQuestion(uid, {{"session_id", session_id}, {"expected_total_attempts", 1}});
    Check(Text(repair.at("question").as_object(), "id") == card.id &&
              repair.at("question").as_object().at("assisted").as_bool(),
          "principle help skipped repair or lost assistance");
    const auto fixed =
        restarted.SubmitAnswer(uid, {{"question_id", card.id}, {"session_id", session_id}, {"answer", correct_answer}});
    const auto& fixed_feedback = fixed.at("session").as_object().at("feedback").as_object();
    const auto summary = repository.LoadProgressSummary(uid);
    Check(fixed_feedback.at("correct").as_bool() && fixed_feedback.at("assisted").as_bool() &&
              fixed_feedback.at("xp_gained").as_int64() == 0 && summary.xp_total == 0 &&
              summary.answers.correct_first_answers == 0 && repository.LoadCleanQuestionFamilies(uid).empty(),
          "mechanism-assisted repair generated XP or clean evidence");
    static_cast<void>(restarted.StopSession(uid));
  }
}
}  // namespace

void CheckWrongAnswerHelp(const char* url) {
  const auto cards = LoadQuestions("content/questions");
  auto pool = std::make_shared<DatabaseConnectionPool>(url, 3);
  AttemptRepository repository(pool);
  std::unique_ptr<PGconn, decltype(&PQfinish)> db(PQconnectdb(url), PQfinish);
  Check(db && PQstatus(db.get()) == CONNECTION_OK, "answer-help disposable database is unavailable");
  const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
  auto uid = -6'000'000'000 - static_cast<std::int64_t>(stamp % 100'000'000);

  for (int scenario = 0; scenario < 6; ++scenario, --uid) {
    const auto target = std::ranges::find_if(cards, [scenario](const auto& card) {
      if (!std::holds_alternative<SingleChoiceTask>(card.task))
        return false;
      return scenario == 1 ? card.kind == QuestionKind::kCodeReading && FindTheoryForSkill(card.skill_id) == nullptr &&
                                 ReferencesForQuestion(card).empty()
                           : card.kind == QuestionKind::kTheory && FindTheoryForSkill(card.skill_id) != nullptr;
    });
    Check(target != cards.end(), "missing real wrong-answer fixture");
    auto card = *target;
    // Synthetic legacy gaps exercise fallbacks; the public bank is never edited.
    if (scenario == 2 || scenario == 3) {
      auto content = boost::json::parse(card.content_snapshot_json).as_object();
      card.explanation.clear();
      content["explanation"] = "";
      if (scenario == 3)
        content.erase("explanation");
      Identify(card, content);
    }
    std::vector<Question> fixtures{card};
    for (const auto& other : cards) {
      if (fixtures.size() == 5)
        break;
      if (other.level == card.level && other.topic != card.topic)
        fixtures.push_back(other);
    }
    Check(fixtures.size() == 5, "not enough cross-topic level fixtures");
    QuestionBank bank(fixtures);
    repository.EnsureUser(uid);
    const auto level = std::to_string(card.level);
    Sql(db.get(), "UPDATE users SET adaptive_level = " + level + ", unlocked_level = " + level +
                      " WHERE user_id = " + std::to_string(uid));
    MiniAppService service(bank, pool);
    const boost::json::object start{{"modes", boost::json::array{scenario == 1 ? "code" : "theory"}},
                                    {"topics", boost::json::array{card.topic}},
                                    {"level", card.level}};
    const auto initial = service.StartSession(uid, start);
    Check(Text(initial.at("question").as_object(), "id") == card.id, "wrong fixture was selected");
    const auto session_id = initial.at("session_id");
    const boost::json::object help_request{{"question_id", card.id},
                                           {"session_id", session_id},
                                           {"content_revision", card.content_revision},
                                           {"kind", "answer"}};
    Reject([&] { static_cast<void>(service.OpenTheory(uid, help_request)); });
    const auto& task = std::get<SingleChoiceTask>(card.task);
    const auto wrong =
        std::ranges::find_if(task.options, [&](const auto& option) { return option.id != task.correct_option_id; });
    const auto correct = std::ranges::find(task.options, task.correct_option_id, &AnswerOption::id);
    const boost::json::object answer{{"question_id", card.id}, {"session_id", session_id}, {"answer", wrong->id}};
    const auto receipt = service.SubmitAnswer(uid, answer);
    const auto feedback = receipt.at("session").as_object().at("feedback").as_object();
    Check(!feedback.at("correct").as_bool() && !feedback.at("completed").as_bool() &&
              !feedback.contains("explanation") && !feedback.contains("correct_answer") &&
              !feedback.contains("source") && feedback.at("answer_available").as_bool() &&
              Text(feedback, "answer") == wrong->text,
          "initial wrong feedback disclosed the answer or failed to offer explicit help");
    const auto before = repository.LoadActiveTrainingSession(uid);
    Check(service.SubmitAnswer(uid, answer) == receipt, "answer redelivery changed feedback");
    Check(repository.LoadActiveTrainingSession(uid)->state_version == before->state_version,
          "answer redelivery wrote another attempt");
    auto invalid = help_request;
    invalid["session_id"] = session_id.as_int64() + 1;
    Reject([&] { static_cast<void>(service.OpenTheory(uid, invalid)); });
    invalid = help_request;
    invalid["question_id"] = "another-card";
    Reject([&] { static_cast<void>(service.OpenTheory(uid, invalid)); });
    invalid = help_request;
    invalid["kind"] = "silently-disclose";
    Reject([&] { static_cast<void>(service.OpenTheory(uid, invalid)); });
    invalid = help_request;
    invalid["content_revision"] = std::string(64, '0');
    Reject([&] { static_cast<void>(service.OpenTheory(uid, invalid)); });
    if (scenario == 5) {
      // Revisionless v2 feedback predates archived answer disclosure. Its existing
      // principle request must still work, but must never reveal today's key.
      auto legacy = boost::json::parse(before->state_json).as_object();
      auto& saved_feedback = legacy.at("last_feedback").as_object();
      saved_feedback.erase("content_revision");
      saved_feedback.erase("answer_available");
      saved_feedback.erase("explanation_available");
      static_cast<void>(repository.UpdateActiveTrainingSession(
          uid, before->session_id, before->state_version, before->content_version, boost::json::serialize(legacy)));
      MiniAppService restored(bank, pool);
      const boost::json::object request{{"question_id", card.id}};
      const auto principle = restored.OpenTheory(uid, request);
      const auto& response_feedback = principle.at("session").as_object().at("feedback").as_object();
      Check(principle.contains("theory") && response_feedback.at("theory_viewed").as_bool() &&
                !response_feedback.contains("correct_answer") && !response_feedback.contains("explanation") &&
                !response_feedback.at("answer_available").as_bool(),
            "revisionless legacy feedback lost principle help or disclosed today's answer");
      const auto saved = repository.LoadActiveTrainingSession(uid);
      Check(restored.OpenTheory(uid, request) == principle &&
                repository.LoadActiveTrainingSession(uid)->state_version == saved->state_version,
            "legacy principle replay duplicated assistance");
      Reject([&] { static_cast<void>(restored.OpenTheory(uid, {{"question_id", card.id}, {"kind", "answer"}})); });
      Check(repository.LoadActiveTrainingSession(uid)->state_version == saved->state_version,
            "revisionless answer request wrote state");
      static_cast<void>(restored.StopSession(uid));
      continue;
    }
    if (scenario == 1) {
      Reject([&] { static_cast<void>(service.OpenTheory(uid, {{"question_id", card.id}})); });
      Check(repository.LoadActiveTrainingSession(uid)->state_version == before->state_version,
            "unavailable principle help marked assistance");
    } else if (scenario == 0) {
      const auto principle = service.OpenTheory(uid, {{"question_id", card.id}});
      Check(principle.contains("theory") &&
                !principle.at("session").as_object().at("feedback").as_object().contains("correct_answer") &&
                !principle.at("session").as_object().at("feedback").as_object().contains("explanation"),
            "principle-only help secretly disclosed the answer");
      const auto principle_saved = repository.LoadActiveTrainingSession(uid);
      Check(service.OpenTheory(uid, {{"question_id", card.id}}) == principle &&
                repository.LoadActiveTrainingSession(uid)->state_version == principle_saved->state_version,
            "reopening a principle duplicated assistance");
    } else if (scenario == 3) {
      auto unavailable = boost::json::parse(before->state_json).as_object();
      unavailable["current_content_revision"] = std::string(64, '0');
      unavailable.at("last_feedback").as_object()["content_revision"] = std::string(64, '0');
      const auto version = repository.UpdateActiveTrainingSession(
          uid, before->session_id, before->state_version, before->content_version, boost::json::serialize(unavailable));
      auto missing = help_request;
      missing["content_revision"] = std::string(64, '0');
      Reject([&] { static_cast<void>(service.OpenTheory(uid, missing)); });
      const auto rejected = repository.LoadActiveTrainingSession(uid);
      const auto rejected_state = boost::json::parse(rejected->state_json).as_object();
      Check(rejected->state_version == version && rejected_state.at("theory_viewed_question_ids").as_array().empty(),
            "missing archived help returned content or marked assistance");
      static_cast<void>(repository.UpdateActiveTrainingSession(uid, before->session_id, version,
                                                               before->content_version, before->state_json));
    }

    if (scenario == 4) {
      // Old v2 feedback predates availability flags. Keep its pinned revision.
      const auto saved = repository.LoadActiveTrainingSession(uid);
      auto legacy = boost::json::parse(saved->state_json).as_object();
      legacy.at("last_feedback").as_object().erase("answer_available");
      legacy.at("last_feedback").as_object().erase("explanation_available");
      static_cast<void>(repository.UpdateActiveTrainingSession(uid, saved->session_id, saved->state_version,
                                                               saved->content_version, boost::json::serialize(legacy)));
      // Today's same-ID card has a different key and explanation, with stable option IDs.
      auto revised = card;
      auto content = boost::json::parse(card.content_snapshot_json).as_object();
      std::get<SingleChoiceTask>(revised.task).correct_option_id = wrong->id;
      revised.explanation = "another revision that must never replace saved feedback";
      content.at("task").as_object()["correct_option_id"] = wrong->id;
      content["explanation"] = revised.explanation;
      Identify(revised, content);
      fixtures.front() = revised;
    }
    QuestionBank later_bank(fixtures);
    MiniAppService writer(later_bank, pool);
    const auto restored_wrong = writer.CurrentSession(uid).at("feedback").as_object();
    Check(restored_wrong.at("answer_available").as_bool() && !restored_wrong.contains("correct_answer"),
          "restoring legacy wrong feedback hid help or leaked the answer");
    const auto opened = writer.OpenTheory(uid, help_request);
    const auto disclosed = opened.at("session").as_object().at("feedback").as_object();
    Check(!disclosed.at("correct").as_bool() && !disclosed.at("completed").as_bool() &&
              disclosed.at("answer_revealed").as_bool() && Text(disclosed, "correct_answer") == correct->text &&
              Text(disclosed, "explanation") == card.explanation &&
              Text(disclosed, "content_revision") == card.content_revision,
          "explicit help did not disclose the pinned revision while preserving the wrong result");
    const auto persisted = repository.LoadActiveTrainingSession(uid);
    const auto state = boost::json::parse(persisted->state_json).as_object();
    Check(state.at("theory_viewed_question_ids").as_array().size() == 1 &&
              state.at("last_feedback").as_object() == disclosed,
          "answer was returned before assistance and disclosure were persisted together");
    // The first service still has the pre-opening cache: model a committed opening
    // whose acknowledgement was lost, without pretending to test a real network failure.
    Check(service.OpenTheory(uid, help_request) == opened && writer.OpenTheory(uid, help_request) == opened &&
              repository.LoadActiveTrainingSession(uid)->state_version == persisted->state_version,
          "repeated opening or lost acknowledgement duplicated assistance");
    Check(repository.LoadProgressSummary(uid).completed_count == 0 && repository.LoadCleanQuestionFamilies(uid).empty(),
          "opening explanation counted as completion or clean evidence");

    MiniAppService restarted(bank, pool);
    Check(restarted.CurrentSession(uid).at("feedback") == disclosed, "restart forgot answer disclosure");
    const auto another = restarted.StartSession(uid, start);
    const auto resumed = restarted.ResumeSession(uid, {{"session_id", session_id}});
    Check(resumed.at("feedback") == disclosed, "saved training restore lost help");
    const boost::json::object next{{"session_id", session_id}, {"expected_total_attempts", 1}};
    const auto repair = restarted.NextQuestion(uid, next);
    Check(Text(repair.at("question").as_object(), "id") == card.id &&
              repair.at("question").as_object().at("assisted").as_bool() &&
              repair.at("progress").as_object().at("attempt").to_number<std::int64_t>() == 2,
          "help skipped repair or forgot assistance");
    Check(restarted.NextQuestion(uid, next) == repair, "retrying Next advanced repair twice");
    std::int64_t total_attempts = 2;
    if (scenario == 1) {
      const auto wrong_again = restarted.SubmitAnswer(uid, answer).at("session").as_object().at("feedback").as_object();
      Check(!wrong_again.at("correct").as_bool() && wrong_again.at("assisted").as_bool() &&
                !wrong_again.contains("correct_answer"),
            "repair error lost assistance or automatically disclosed the key");
      const auto opening = restarted.OpenTheory(uid, help_request);
      const auto reopened = opening.at("session").as_object().at("feedback").as_object();
      Check(Text(reopened, "correct_answer") == correct->text && reopened.at("answer_revealed").as_bool(),
            "answer could not be reopened after another error");
      const auto retry = restarted.NextQuestion(uid, {{"session_id", session_id}, {"expected_total_attempts", 2}});
      Check(retry.at("question").as_object().at("assisted").as_bool(), "another repair forgot assistance");
      total_attempts = 3;
    }
    const boost::json::object fixed_answer{
        {"session_id", session_id}, {"question_id", card.id}, {"answer", task.correct_option_id}};
    const auto fixed = restarted.SubmitAnswer(uid, fixed_answer);
    const auto fixed_feedback = fixed.at("session").as_object().at("feedback").as_object();
    Check(fixed_feedback.at("correct").as_bool() && fixed_feedback.at("assisted").as_bool() &&
              fixed_feedback.at("xp_gained").as_int64() == 0 && Text(fixed_feedback, "explanation") == card.explanation,
          "assisted repair earned XP or lost completed disclosure");
    Check(restarted.SubmitAnswer(uid, fixed_answer) == fixed, "duplicate assisted answer changed progress");
    Reject([&] { static_cast<void>(restarted.OpenTheory(uid, help_request)); });
    const auto summary = repository.LoadProgressSummary(uid);
    Check(summary.completed_count == 1 && summary.xp_total == 0 && summary.answers.correct_first_answers == 0 &&
              repository.LoadCleanQuestionFamilies(uid).empty(),
          "assisted answer created clean learning evidence");
    const boost::json::object complete{{"session_id", session_id}, {"expected_total_attempts", total_attempts}};
    const auto done = restarted.NextQuestion(uid, complete);
    Check(Text(done, "status") == "completed" && restarted.NextQuestion(uid, complete) == done &&
              done.at("learning").as_object().at("theory_opened").to_number<std::int64_t>() == 1,
          "completion redelivery counted disclosure or progress twice");
    static_cast<void>(restarted.ResumeSession(uid, {{"session_id", another.at("session_id")}}));
    const auto clean = restarted.SubmitAnswer(uid, {{"question_id", card.id}, {"answer", task.correct_option_id}});
    const auto clean_feedback = clean.at("session").as_object().at("feedback").as_object();
    Check(clean_feedback.at("correct").as_bool() && !clean_feedback.at("assisted").as_bool() &&
              Text(clean_feedback, "correct_answer") == correct->text &&
              Text(clean_feedback, "explanation") == card.explanation,
          "ordinary correct-answer disclosure regressed");
    static_cast<void>(restarted.StopSession(uid));
  }
  CheckMechanismHelp(cards, pool, repository, db.get(), uid);
}
