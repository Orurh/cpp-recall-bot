#include "miniapp/miniapp_service.h"

#include "app/quiz_session_service.h"
#include "app/training_mode.h"
#include "learning/level_progress_policy.h"
#include "learning/module_catalog.h"
#include "learning/progress_summary.h"
#include "learning/skill_catalog.h"
#include "quiz/matching_round.h"
#include "quiz/question.h"
#include "quiz/quiz_service.h"
#include "quiz/topic_catalog.h"
#include "storage/attempt_repository.h"

#include <boost/json.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <random>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <variant>
#include <vector>

namespace {

constexpr std::size_t kMinimumQuestionsForSelectableLevel = 5;

std::string CopyString(const boost::json::string& value) {
  return {value.data(), value.size()};
}

std::string RequireString(const boost::json::object& object, std::string_view key, std::size_t max_size = 4096) {
  const auto* value = object.if_contains(key);
  if (value == nullptr || !value->is_string()) {
    throw std::invalid_argument(std::string(key) + " must be a string");
  }
  auto result = CopyString(value->as_string());
  if (result.empty() || result.size() > max_size) {
    throw std::invalid_argument(std::string(key) + " has invalid length");
  }
  return result;
}

std::size_t RequireSize(const boost::json::object& object, std::string_view key) {
  const auto* value = object.if_contains(key);
  if (value == nullptr) {
    throw std::invalid_argument(std::string(key) + " is required");
  }
  if (value->is_uint64()) {
    if (value->as_uint64() > std::numeric_limits<std::size_t>::max()) {
      throw std::invalid_argument(std::string(key) + " is too large");
    }
    return static_cast<std::size_t>(value->as_uint64());
  }
  if (value->is_int64() && value->as_int64() >= 0) {
    return static_cast<std::size_t>(value->as_int64());
  }
  throw std::invalid_argument(std::string(key) + " must be non-negative");
}

int RequireLevel(const boost::json::object& object, std::string_view key) {
  const auto* value = object.if_contains(key);
  if (value == nullptr || !value->is_int64()) {
    throw std::invalid_argument(std::string(key) + " must be an integer");
  }
  const auto level = value->as_int64();
  if (level < 1 || level > 10) {
    throw std::invalid_argument(std::string(key) + " must be between 1 and 10");
  }
  return static_cast<int>(level);
}

std::size_t LevelQuestionCount(const QuestionBank& questions, int level) {
  return questions
      .Select(QuestionSelection{
          .topics = {},
          .skill_ids = {},
          .kinds = {},
          .matching_only = false,
          .min_level = level,
          .max_level = level,
      })
      .size();
}

std::string QuestionKindName(QuestionKind kind) {
  switch (kind) {
    case QuestionKind::kTheory:
      return "theory";
    case QuestionKind::kApiKnowledge:
      return "api_knowledge";
    case QuestionKind::kCodeReading:
      return "code_reading";
    case QuestionKind::kDebugging:
      return "debugging";
    case QuestionKind::kImplementation:
      return "implementation";
  }
  throw std::logic_error("unknown question kind");
}

std::string MatchingOutcomeName(MatchingPick outcome) {
  switch (outcome) {
    case MatchingPick::kIgnored:
      return "ignored";
    case MatchingPick::kSelected:
      return "selected";
    case MatchingPick::kMatched:
      return "matched";
    case MatchingPick::kMismatch:
      return "mismatch";
    case MatchingPick::kFinished:
      return "finished";
  }
  throw std::logic_error("unknown matching outcome");
}

std::string TrainingModeName(TrainingContentMode mode) {
  switch (mode) {
    case TrainingContentMode::kTheory:
      return "theory";
    case TrainingContentMode::kApi:
      return "api";
    case TrainingContentMode::kCode:
      return "code";
    case TrainingContentMode::kMatching:
      return "matching";
  }
  throw std::logic_error("unknown training mode");
}

TrainingContentMode ParseTrainingMode(std::string_view value) {
  if (value == "theory")
    return TrainingContentMode::kTheory;
  if (value == "api")
    return TrainingContentMode::kApi;
  if (value == "code")
    return TrainingContentMode::kCode;
  if (value == "matching")
    return TrainingContentMode::kMatching;
  throw std::invalid_argument("unknown training mode");
}

std::vector<TrainingContentMode> ParseModes(const boost::json::object& request) {
  const auto* value = request.if_contains("modes");
  if (value == nullptr || !value->is_array() || value->as_array().empty()) {
    throw std::invalid_argument("modes must be a non-empty array");
  }
  std::vector<TrainingContentMode> modes;
  for (const auto& item : value->as_array()) {
    if (!item.is_string()) {
      throw std::invalid_argument("every mode must be a string");
    }
    const auto mode = ParseTrainingMode(item.as_string());
    if (std::ranges::find(modes, mode) == modes.end()) {
      modes.push_back(mode);
    }
  }
  return modes;
}

std::vector<std::string> ParseTopics(const boost::json::object& request) {
  const auto* value = request.if_contains("topics");
  if (value == nullptr || !value->is_array()) {
    throw std::invalid_argument("topics must be an array");
  }
  std::vector<std::string> topics;
  for (const auto& item : value->as_array()) {
    if (!item.is_string()) {
      throw std::invalid_argument("every topic must be a string");
    }
    auto topic = CopyString(item.as_string());
    if (!IsKnownTopic(topic)) {
      throw std::invalid_argument("unknown training topic");
    }
    if (std::ranges::find(topics, topic) == topics.end()) {
      topics.push_back(std::move(topic));
    }
  }
  return topics;
}

boost::json::object SerializeProgressSummary(const UserProgressSummary& summary) {
  boost::json::array topics;
  topics.reserve(summary.topics.size());
  for (const auto& topic : summary.topics) {
    topics.emplace_back(boost::json::object{
        {"id", topic.topic},
        {"title", std::string(TopicTitle(topic.topic).value_or(topic.topic))},
        {"level", topic.adaptive_level},
        {"evidence", topic.adaptive_evidence},
        {"completed", topic.completed_count},
        {"correct", topic.correct_count},
        {"failed", topic.failed_count},
        {"first_try_correct", topic.first_try_correct_count},
    });
  }

  boost::json::array skills;
  skills.reserve(summary.skills.size());
  std::int64_t learning_skill_count = 0;
  std::int64_t demonstrated_skill_count = 0;
  std::int64_t retained_skill_count = 0;
  std::int64_t recovery_skill_count = 0;
  for (const auto& skill : summary.skills) {
    const auto* definition = FindSkill(skill.skill_id);
    switch (skill.mastery_status) {
      case SkillMasteryStatus::kEncountered:
      case SkillMasteryStatus::kApplying:
        ++learning_skill_count;
        break;
      case SkillMasteryStatus::kDemonstrated:
        ++demonstrated_skill_count;
        break;
      case SkillMasteryStatus::kRetained:
        ++retained_skill_count;
        break;
      case SkillMasteryStatus::kNeedsRecovery:
        ++recovery_skill_count;
        break;
      case SkillMasteryStatus::kUnassessed:
        break;
    }
    skills.emplace_back(boost::json::object{
        {"id", skill.skill_id},
        {"title", definition == nullptr ? skill.skill_id : std::string(definition->title)},
        {"mastery_status", SkillMasteryStatusName(skill.mastery_status)},
        {"successful_families", skill.successful_family_count},
        {"clean_families", skill.clean_family_count},
        {"application_families", skill.application_family_count},
        {"transfer_families", skill.transfer_family_count},
        {"questions_seen", skill.questions_seen},
        {"completed", skill.completed_count},
        {"correct", skill.correct_count},
        {"failed", skill.failed_count},
        {"first_try_correct", skill.first_try_correct_count},
    });
  }

  const auto accuracy =
      summary.completed_count == 0 ? 0 : static_cast<int>((summary.correct_count * 100) / summary.completed_count);

  struct RewardMilestone {
    std::string_view id;
    std::string_view title;
    std::int64_t threshold;
  };
  static constexpr std::array kRewards{
      RewardMilestone{"profile_frames", "Рамки и значки профиля", 150},
      RewardMilestone{"second_preset", "Второй набор настроек тренировки", 400},
      RewardMilestone{"themes", "Дополнительные темы оформления", 800},
      RewardMilestone{"personal_marathon", "Личный тематический марафон", 1'500},
      RewardMilestone{"five_presets", "Пять наборов настроек и витрина достижений", 3'000},
      RewardMilestone{"public_collections", "Публичные подборки после модерации", 5'000},
  };
  boost::json::value next_reward = nullptr;
  std::int64_t previous_threshold = 0;
  for (const auto& reward : kRewards) {
    if (summary.xp_total < reward.threshold) {
      const auto span = reward.threshold - previous_threshold;
      const auto progress = summary.xp_total - previous_threshold;
      next_reward = boost::json::object{
          {"id", reward.id},
          {"title", reward.title},
          {"threshold", reward.threshold},
          {"remaining", reward.threshold - summary.xp_total},
          {"progress_percent", span == 0 ? 100 : static_cast<int>((progress * 100) / span)},
      };
      break;
    }
    previous_threshold = reward.threshold;
  }

  return {
      {"adaptive_level", summary.adaptive_level},
      {"adaptive_evidence", summary.adaptive_evidence},
      {"unlocked_level", summary.unlocked_level},
      {"sessions_started", summary.sessions_started},
      {"sessions_completed", summary.sessions_completed},
      {"questions_seen", summary.questions_seen},
      {"completed", summary.completed_count},
      {"correct", summary.correct_count},
      {"failed", summary.failed_count},
      {"first_try_correct", summary.first_try_correct_count},
      {"due_review", summary.due_review_count},
      {"accuracy", accuracy},
      {"xp", boost::json::object{{"total", summary.xp_total}, {"next_reward", std::move(next_reward)}}},
      {"skill_mastery",
       boost::json::object{
           {"learning", learning_skill_count},
           {"demonstrated", demonstrated_skill_count},
           {"retained", retained_skill_count},
           {"needs_recovery", recovery_skill_count},
       }},
      {"topics", std::move(topics)},
      {"skills", std::move(skills)},
  };
}

std::string OptionText(const Question& question, std::string_view id) {
  const auto find = [id](const auto& task) -> std::string {
    if constexpr (requires { task.options; }) {
      const auto it = std::ranges::find(task.options, id, &AnswerOption::id);
      if (it == task.options.end()) {
        throw std::invalid_argument("unknown answer option");
      }
      return it->text;
    } else {
      throw std::invalid_argument("question does not contain answer options");
    }
  };
  return std::visit(find, question.task);
}

}  // namespace

struct MiniAppService::Impl {
  struct SessionState {
    QuizSession quiz;
    std::int64_t storage_session_id = 0;
    std::vector<TrainingContentMode> modes;
    std::vector<std::string> topics;
    int selected_level = 1;
    bool tracks_level_progress = false;
    std::vector<std::string> option_order;
    std::optional<MatchingRound> matching;
    std::size_t questions_started = 0;
    std::size_t current_attempt = 1;
    std::int64_t xp_gained = 0;
    std::optional<std::string> last_question_id;
    std::optional<boost::json::object> last_feedback;
  };

  Impl(const QuestionBank& question_bank, std::string database_url)
      : questions(question_bank), quiz(question_bank), attempts(std::move(database_url)) {}

  const QuestionBank& questions;
  QuizSessionService quiz;
  AttemptRepository attempts;
  std::unordered_map<std::int64_t, SessionState> sessions;

  SessionState& RequireSession(std::int64_t user_id) {
    const auto it = sessions.find(user_id);
    if (it == sessions.end()) {
      throw std::logic_error("no active Mini App session");
    }
    return it->second;
  }

  void PrepareCurrent(SessionState& session) {
    const auto& question = quiz.CurrentQuestion(session.quiz);
    session.option_order.clear();

    const auto collect_options = [&session](const auto& task) {
      if constexpr (requires { task.options; }) {
        for (const auto& option : task.options) {
          session.option_order.push_back(option.id);
        }
        static thread_local std::mt19937 generator{std::random_device{}()};
        std::ranges::shuffle(session.option_order, generator);
      }
    };
    std::visit(collect_options, question.task);

    session.matching.reset();
    if (const auto* task = std::get_if<MatchingTask>(&question.task)) {
      session.matching.emplace(task->pairs.size());
    }
    session.current_attempt = session.quiz.queue.CurrentAttempts() + 1;
    if (session.current_attempt == 1)
      ++session.questions_started;
    session.quiz.answered = false;
    session.last_question_id.reset();
    session.last_feedback.reset();
  }

  boost::json::object SerializeMatching(const SessionState& session, const MatchingTask& task) const {
    if (!session.matching) {
      throw std::logic_error("matching question has no matching state");
    }
    const auto& round = *session.matching;
    boost::json::array left;
    boost::json::array right;
    left.reserve(round.Size());
    right.reserve(round.Size());

    for (std::size_t position = 0; position < round.Size(); ++position) {
      const auto left_pair = round.PairIndex(MatchingSide::kLeft, position);
      const auto right_pair = round.PairIndex(MatchingSide::kRight, position);
      left.emplace_back(boost::json::object{
          {"position", position},
          {"text", task.pairs.at(left_pair).left},
          {"matched", round.IsMatched(left_pair)},
          {"selected", round.IsSelected(MatchingSide::kLeft, left_pair)},
      });
      right.emplace_back(boost::json::object{
          {"position", position},
          {"text", task.pairs.at(right_pair).right},
          {"matched", round.IsMatched(right_pair)},
          {"selected", round.IsSelected(MatchingSide::kRight, right_pair)},
      });
    }

    return {
        {"left", std::move(left)},         {"right", std::move(right)},    {"revision", round.Revision()},
        {"matched", round.MatchedCount()}, {"mistakes", round.Mistakes()}, {"last_mismatch", round.LastMismatch()},
        {"total", round.Size()},
    };
  }

  boost::json::object SerializeQuestion(const SessionState& session) const {
    const auto& question = quiz.CurrentQuestion(session.quiz);
    boost::json::array tags;
    for (const auto& tag : question.tags)
      tags.emplace_back(tag);

    boost::json::object task;
    if (const auto* single = std::get_if<SingleChoiceTask>(&question.task)) {
      task["type"] = "single_choice";
      boost::json::array options;
      for (const auto& id : session.option_order) {
        options.emplace_back(boost::json::object{{"id", id}, {"text", OptionText(question, id)}});
      }
      task["options"] = std::move(options);
      static_cast<void>(single);
    } else if (const auto* multiple = std::get_if<MultipleChoiceTask>(&question.task)) {
      task["type"] = "multiple_choice";
      boost::json::array options;
      for (const auto& id : session.option_order) {
        options.emplace_back(boost::json::object{{"id", id}, {"text", OptionText(question, id)}});
      }
      task["options"] = std::move(options);
      static_cast<void>(multiple);
    } else if (std::holds_alternative<ShortAnswerTask>(question.task)) {
      task["type"] = "short_answer";
    } else if (const auto* matching = std::get_if<MatchingTask>(&question.task)) {
      task["type"] = "matching";
      task["board"] = SerializeMatching(session, *matching);
    }

    boost::json::object result{
        {"id", question.id},
        {"topic", question.topic},
        {"topic_title", std::string(TopicTitle(question.topic).value_or(question.topic))},
        {"skill_id", question.skill_id},
        {"question_family_id", question.question_family_id},
        {"evidence_type", QuestionEvidenceTypeName(question.evidence_type)},
        {"transfer", question.transfer},
        {"kind", QuestionKindName(question.kind)},
        {"level", question.level},
        {"standard", question.standard},
        {"tags", std::move(tags)},
        {"text", question.text},
        {"task", std::move(task)},
    };
    if (question.code)
      result["code"] = *question.code;
    return result;
  }

  boost::json::object SerializeSession(const SessionState& session) const {
    boost::json::array modes;
    for (const auto mode : session.modes) {
      modes.emplace_back(TrainingModeName(mode));
    }
    boost::json::array topics;
    for (const auto& topic : session.topics)
      topics.emplace_back(topic);

    boost::json::object result{
        {"status", "active"},
        {"phase", session.quiz.answered ? "feedback" : "question"},
        {"modes", std::move(modes)},
        {"topics", std::move(topics)},
        {"selected_level", session.selected_level},
        {"tracks_level_progress", session.tracks_level_progress},
        {"progress",
         boost::json::object{
             {"question_number", session.questions_started},
             {"session_goal", session.quiz.session_goal},
             {"completed", session.quiz.completed_question_ids.size()},
             {"attempt", session.current_attempt},
             {"total_attempts", session.quiz.total_attempts},
             {"correct_attempts", session.quiz.correct_attempts},
             {"current_streak", session.quiz.current_streak},
             {"best_streak", session.quiz.best_streak},
             {"adaptive_level", session.quiz.adaptive_state.level},
             {"xp_gained", session.xp_gained},
         }},
    };
    if (session.quiz.answered) {
      if (!session.last_feedback) {
        throw std::logic_error("answered session has no feedback");
      }
      result["feedback"] = *session.last_feedback;
    } else {
      result["question"] = SerializeQuestion(session);
      const auto& question = quiz.CurrentQuestion(session.quiz);
      result.at("progress").as_object()["topic_level"] = quiz.TopicLevel(session.quiz, question.topic);
    }
    return result;
  }

  boost::json::object ApplySubmission(std::int64_t user_id, SessionState& session, const Question& question,
                                      std::string_view storage_answer, std::string display_answer,
                                      AnswerSubmission submission) {
    if (submission.result == AnswerResult::kUnknownOption) {
      throw std::invalid_argument("answer contains an unknown option");
    }

    const int previous_level = session.quiz.adaptive_state.level;
    const int previous_topic_level = quiz.TopicLevel(session.quiz, question.topic);
    const auto previous_topic_state = quiz.TopicState(session.quiz, question.topic);

    if (!CanApplyLevelEvidence(session.tracks_level_progress, session.selected_level, previous_level)) {
      submission.session.adaptive_state = session.quiz.adaptive_state;
    }
    if (!CanApplyLevelEvidence(session.tracks_level_progress, session.selected_level, previous_topic_level)) {
      submission.session.topic_states.insert_or_assign(question.topic, previous_topic_state);
    }

    const auto next_topic_state = quiz.TopicState(submission.session, question.topic);
    const bool correct = submission.result == AnswerResult::kCorrect;

    const auto recorded = attempts.RecordSubmission(SubmissionRecord{
        .user_id = user_id,
        .session_id = session.storage_session_id,
        .question_id = question.id,
        .topic = question.topic,
        .skill_id = question.skill_id,
        .question_family_id = question.question_family_id,
        .evidence_type = question.evidence_type,
        .transfer = question.transfer,
        .answer = storage_answer,
        .correct = correct,
        .completed = submission.completed,
        .attempt_number = submission.attempts,
        .topic_level = next_topic_state.level,
        .topic_level_evidence = next_topic_state.evidence,
        .adaptive_level = submission.session.adaptive_state.level,
        .adaptive_level_evidence = submission.session.adaptive_state.evidence,
    });
    session.xp_gained += recorded.xp_gained;

    session.quiz = std::move(submission.session);
    boost::json::object feedback{
        {"question_id", question.id},
        {"correct", correct},
        {"completed", submission.completed},
        {"attempts", submission.attempts},
        {"answer", std::move(display_answer)},
        {"previous_topic_level", previous_topic_level},
        {"topic_level", next_topic_state.level},
        {"previous_adaptive_level", previous_level},
        {"adaptive_level", session.quiz.adaptive_state.level},
        {"xp_gained", recorded.xp_gained},
    };
    if (submission.completed) {
      feedback["explanation"] = question.explanation;
      if (question.source)
        feedback["source"] = *question.source;
    }
    if (recorded.skill_mastery) {
      const auto& mastery = *recorded.skill_mastery;
      const auto* skill = FindSkill(mastery.skill_id);
      feedback["skill_mastery"] = boost::json::object{
          {"skill_id", mastery.skill_id},
          {"skill_title", skill == nullptr ? mastery.skill_id : std::string(skill->title)},
          {"previous", SkillMasteryStatusName(mastery.previous)},
          {"current", SkillMasteryStatusName(mastery.current)},
          {"successful_families", mastery.successful_family_count},
          {"clean_families", mastery.clean_family_count},
          {"application_families", mastery.application_family_count},
          {"transfer_families", mastery.transfer_family_count},
      };
    }

    session.last_question_id = question.id;
    session.last_feedback = feedback;
    return feedback;
  }
};

MiniAppService::MiniAppService(const QuestionBank& questions, std::string database_url)
    : impl_(std::make_unique<Impl>(questions, std::move(database_url))) {}

MiniAppService::~MiniAppService() = default;

boost::json::object MiniAppService::Bootstrap(const TelegramMiniAppUser& user) {
  impl_->attempts.EnsureUser(user.id);
  const auto summary = impl_->attempts.LoadProgressSummary(user.id);
  boost::json::array modes{
      boost::json::object{{"id", "theory"}, {"title", "Теория"}},
      boost::json::object{{"id", "api"}, {"title", "Библиотека C++ (API/STL)"}},
      boost::json::object{{"id", "code"}, {"title", "Код"}},
      boost::json::object{{"id", "matching"}, {"title", "Только сопоставление"}},
  };
  boost::json::array topics;
  for (const auto& topic : TrainingTopics()) {
    topics.emplace_back(boost::json::object{{"id", std::string(topic.id)}, {"title", std::string(topic.title)}});
  }
  boost::json::array skills;
  for (const auto& skill : SkillCatalog()) {
    boost::json::array prerequisites;
    for (const auto prerequisite : skill.prerequisites) {
      prerequisites.emplace_back(prerequisite);
    }
    skills.emplace_back(boost::json::object{
        {"id", skill.id},
        {"topic", skill.topic},
        {"title", skill.title},
        {"prerequisites", std::move(prerequisites)},
    });
  }
  boost::json::array modules;
  for (const auto& module : LearningModuleCatalog()) {
    boost::json::array module_skills;
    for (const auto skill_id : module.skill_ids) {
      module_skills.emplace_back(skill_id);
    }
    modules.emplace_back(boost::json::object{
        {"id", module.id},
        {"title", module.title},
        {"description", module.description},
        {"skill_ids", std::move(module_skills)},
    });
  }

  boost::json::array levels;
  for (int level = 1; level <= 10; ++level) {
    const auto question_count = LevelQuestionCount(impl_->questions, level);
    if (question_count == 0) {
      continue;
    }
    const bool ready = question_count >= kMinimumQuestionsForSelectableLevel;
    levels.emplace_back(boost::json::object{
        {"level", level},
        {"question_count", question_count},
        {"ready", ready},
        {"unlocked", ready && level <= summary.unlocked_level},
        {"current", level == summary.adaptive_level},
    });
  }

  boost::json::object result{
      {"user",
       boost::json::object{
           {"id", user.id},
           {"first_name", user.first_name},
           {"last_name", user.last_name},
           {"username", user.username},
       }},
      {"modes", std::move(modes)},
      {"topics", std::move(topics)},
      {"skills", std::move(skills)},
      {"modules", std::move(modules)},
      {"levels", std::move(levels)},
      {"summary", SerializeProgressSummary(summary)},
  };
  const auto session = impl_->sessions.find(user.id);
  result["session"] = session == impl_->sessions.end() ? boost::json::value(nullptr)
                                                       : boost::json::value(impl_->SerializeSession(session->second));
  return result;
}

boost::json::object MiniAppService::StartSession(std::int64_t user_id, const boost::json::object& request) {
  auto modes = ParseModes(request);
  auto topics = ParseTopics(request);
  const auto selected_level = RequireLevel(request, "level");
  const auto unlocked_level = impl_->attempts.GetUnlockedLevel(user_id);
  if (selected_level > unlocked_level) {
    throw std::invalid_argument("этот уровень ещё не открыт");
  }
  if (LevelQuestionCount(impl_->questions, selected_level) < kMinimumQuestionsForSelectableLevel) {
    throw std::invalid_argument("для этого уровня пока недостаточно вопросов");
  }
  auto selection = QuestionSelectionForModes(modes, topics);
  selection.min_level = selected_level;
  selection.max_level = selected_level;
  if (!impl_->quiz.HasQuestions(selection)) {
    throw std::invalid_argument("на выбранном уровне нет вопросов для этих тем и форматов");
  }

  if (const auto existing = impl_->sessions.find(user_id); existing != impl_->sessions.end()) {
    impl_->attempts.FinishSession(existing->second.storage_session_id, "restarted");
    impl_->sessions.erase(existing);
  }
  impl_->attempts.FinishOpenSessionsWithModePrefix(user_id, "miniapp:", "miniapp_restarted");

  auto adaptive_state = impl_->attempts.GetAdaptiveState(user_id);
  auto topic_states = impl_->attempts.LoadTopicStates(user_id);
  auto review_states = impl_->attempts.LoadReviewStates(user_id);
  auto quiz = impl_->quiz.Start(selection, adaptive_state, std::move(topic_states), std::move(review_states));
  std::string storage_mode = "miniapp:" + TrainingModeStorageName(modes) + ":level-" + std::to_string(selected_level);
  Impl::SessionState state{
      .quiz = std::move(quiz),
      .storage_session_id = impl_->attempts.StartSession(user_id, storage_mode),
      .modes = std::move(modes),
      .topics = std::move(topics),
      .selected_level = selected_level,
      .tracks_level_progress = IsProgressionPractice(selected_level, unlocked_level),
      .option_order = {},
      .matching = std::nullopt,
      .questions_started = 0,
      .current_attempt = 1,
      .xp_gained = 0,
      .last_question_id = std::nullopt,
      .last_feedback = std::nullopt,
  };
  impl_->PrepareCurrent(state);
  auto [it, inserted] = impl_->sessions.emplace(user_id, std::move(state));
  static_cast<void>(inserted);
  return impl_->SerializeSession(it->second);
}

boost::json::object MiniAppService::CurrentSession(std::int64_t user_id) {
  return impl_->SerializeSession(impl_->RequireSession(user_id));
}

boost::json::object MiniAppService::SubmitAnswer(std::int64_t user_id, const boost::json::object& request) {
  auto& session = impl_->RequireSession(user_id);
  const auto question_id = RequireString(request, "question_id", 256);
  if (session.quiz.answered) {
    if (session.last_question_id == question_id && session.last_feedback) {
      return {{"session", impl_->SerializeSession(session)}};
    }
    throw std::logic_error("the current question is already answered");
  }
  const auto& question = impl_->quiz.CurrentQuestion(session.quiz);
  if (question.id != question_id) {
    throw std::logic_error("stale question id");
  }

  std::optional<AnswerSubmission> submission;
  std::string storage_answer;
  std::string display_answer;

  if (std::holds_alternative<SingleChoiceTask>(question.task)) {
    const auto answer = RequireString(request, "answer", 256);
    submission.emplace(impl_->quiz.SubmitAnswer(session.quiz, answer));
    storage_answer = boost::json::serialize(answer);
    display_answer = OptionText(question, answer);
  } else if (std::holds_alternative<MultipleChoiceTask>(question.task)) {
    const auto* value = request.if_contains("answer");
    if (value == nullptr || !value->is_array() || value->as_array().empty()) {
      throw std::invalid_argument("answer must be a non-empty array");
    }
    std::vector<std::string> answers;
    std::unordered_set<std::string> unique;
    boost::json::array stored;
    for (const auto& item : value->as_array()) {
      if (!item.is_string()) {
        throw std::invalid_argument("every answer must be a string");
      }
      auto answer = CopyString(item.as_string());
      if (!unique.insert(answer).second) {
        throw std::invalid_argument("duplicate answer option");
      }
      answers.push_back(answer);
    }
    std::ranges::sort(answers);
    for (const auto& answer : answers)
      stored.emplace_back(answer);
    submission.emplace(impl_->quiz.SubmitAnswer(session.quiz, std::span<const std::string>(answers)));
    storage_answer = boost::json::serialize(stored);
    for (const auto& answer : answers) {
      if (!display_answer.empty())
        display_answer += ", ";
      display_answer += OptionText(question, answer);
    }
  } else if (std::holds_alternative<ShortAnswerTask>(question.task)) {
    const auto answer = RequireString(request, "answer", 4096);
    submission.emplace(impl_->quiz.SubmitAnswer(session.quiz, answer));
    storage_answer = boost::json::serialize(answer);
    display_answer = answer;
  } else {
    throw std::invalid_argument("matching answers must use the matching endpoint");
  }

  impl_->ApplySubmission(user_id, session, question, storage_answer, std::move(display_answer), std::move(*submission));
  return {{"session", impl_->SerializeSession(session)}};
}

boost::json::object MiniAppService::SelectMatching(std::int64_t user_id, const boost::json::object& request) {
  auto& session = impl_->RequireSession(user_id);
  const auto question_id = RequireString(request, "question_id", 256);
  if (session.quiz.answered) {
    if (session.last_question_id == question_id && session.last_feedback) {
      return {{"outcome", "finished"}, {"session", impl_->SerializeSession(session)}};
    }
    throw std::logic_error("the current question is already answered");
  }
  const auto& question = impl_->quiz.CurrentQuestion(session.quiz);
  if (question.id != question_id) {
    throw std::logic_error("stale question id");
  }
  const auto* task = std::get_if<MatchingTask>(&question.task);
  if (task == nullptr || !session.matching) {
    throw std::invalid_argument("current question is not matching");
  }

  const auto side_text = RequireString(request, "side", 8);
  const auto side = side_text == "left"    ? MatchingSide::kLeft
                    : side_text == "right" ? MatchingSide::kRight
                                           : throw std::invalid_argument("unknown matching side");
  const auto outcome =
      session.matching->Select(side, RequireSize(request, "position"), RequireSize(request, "revision"));
  if (outcome == MatchingPick::kFinished) {
    const auto mistakes = session.matching->Mistakes();
    const auto storage_answer = boost::json::serialize(boost::json::object{
        {"source", "mini_app_api"},
        {"matched", session.matching->MatchedCount()},
        {"mistakes", mistakes},
    });
    const auto display_answer = std::to_string(task->pairs.size()) + "/" + std::to_string(task->pairs.size()) +
                                " пар, ошибок: " + std::to_string(mistakes);
    impl_->ApplySubmission(user_id, session, question, storage_answer, display_answer,
                           impl_->quiz.SubmitMatchingAnswer(session.quiz, mistakes == 0));
  }

  return {
      {"outcome", MatchingOutcomeName(outcome)},
      {"session", impl_->SerializeSession(session)},
  };
}

boost::json::object MiniAppService::NextQuestion(std::int64_t user_id) {
  auto& session = impl_->RequireSession(user_id);
  if (!session.quiz.answered) {
    throw std::logic_error("answer the current question first");
  }

  if (session.quiz.queue.Empty()) {
    session.quiz = impl_->quiz.Refill(session.quiz);
  }
  if (session.quiz.queue.Empty()) {
    const auto storage_session_id = session.storage_session_id;
    const auto xp_gained = session.xp_gained;
    impl_->attempts.FinishSession(storage_session_id, "completed");
    impl_->sessions.erase(user_id);
    return {
        {"status", "completed"},
        {"xp_gained", xp_gained},
        {"summary", SerializeProgressSummary(impl_->attempts.LoadProgressSummary(user_id))},
    };
  }

  impl_->PrepareCurrent(session);
  return impl_->SerializeSession(session);
}

boost::json::object MiniAppService::StopSession(std::int64_t user_id) {
  if (const auto it = impl_->sessions.find(user_id); it != impl_->sessions.end()) {
    impl_->attempts.FinishSession(it->second.storage_session_id, "miniapp_stopped");
    impl_->sessions.erase(it);
  }
  return {
      {"status", "stopped"},
      {"summary", SerializeProgressSummary(impl_->attempts.LoadProgressSummary(user_id))},
  };
}
