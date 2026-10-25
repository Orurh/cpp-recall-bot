#include "miniapp/miniapp_service.h"

#include "app/quiz_session_service.h"
#include "app/training_mode.h"
#include "learning/level_progress_policy.h"
#include "learning/module_catalog.h"
#include "learning/progress_summary.h"
#include "learning/reward_catalog.h"
#include "learning/skill_catalog.h"
#include "learning/theory_catalog.h"
#include "learning/training_recommendation.h"
#include "miniapp/session_snapshot.h"
#include "quiz/matching_round.h"
#include "quiz/question.h"
#include "quiz/quiz_service.h"
#include "quiz/topic_catalog.h"
#include "runtime/keyed_mutex.h"
#include "storage/attempt_repository.h"

#include <spdlog/spdlog.h>
#include <boost/json.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <map>
#include <mutex>
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

bool OptionalBoolean(const boost::json::object& object, std::string_view key) {
  const auto* value = object.if_contains(key);
  if (value == nullptr) {
    return false;
  }
  if (!value->is_bool()) {
    throw std::invalid_argument(std::string(key) + " must be a boolean");
  }
  return value->as_bool();
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
          .preferred_question_ids = {},
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

boost::json::value Percentage(std::int64_t numerator, std::int64_t denominator) {
  if (denominator == 0)
    return nullptr;
  return static_cast<double>(
      std::round(1000.0L * static_cast<long double>(numerator) / static_cast<long double>(denominator)) / 10.0L);
}

boost::json::object SerializeAnswerStatistics(const AnswerStatistics& answers) {
  return {{"attempt_count", answers.attempts},
          {"correct_attempt_count", answers.correct_attempts},
          {"first_answer_count", answers.first_answers},
          {"correct_first_answer_count", answers.correct_first_answers},
          {"attempt_accuracy", Percentage(answers.correct_attempts, answers.attempts)},
          {"first_answer_accuracy", Percentage(answers.correct_first_answers, answers.first_answers)}};
}

boost::json::object SerializeProgressSummary(const UserProgressSummary& summary, const QuestionBank& questions) {
  std::unordered_map<std::string, const Question*> known_questions;
  for (const auto& id : questions.QuestionIds())
    known_questions.emplace(id, &questions.Get(id));
  std::map<std::string, AnswerStatistics> topic_answers;
  std::map<std::string, AnswerStatistics> skill_answers;
  std::int64_t unclassified_attempts = 0;
  for (const auto& item : summary.question_answers) {
    const auto found = known_questions.find(item.question_id);
    if (found == known_questions.end()) {
      unclassified_attempts += item.answers.attempts;
      continue;
    }
    topic_answers[found->second->topic] += item.answers;
    skill_answers[found->second->skill_id] += item.answers;
  }
  const auto groups = [](const auto& counts) {
    boost::json::array result;
    for (const auto& [id, answers] : counts) {
      auto item = SerializeAnswerStatistics(answers);
      item["id"] = id;
      result.emplace_back(std::move(item));
    }
    return result;
  };
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

  const auto first_try_rate = summary.completed_count == 0
                                  ? 0
                                  : static_cast<int>((summary.first_try_correct_count * 100) / summary.completed_count);

  boost::json::value next_reward = nullptr;
  std::int64_t previous_threshold = 0;
  for (const auto& reward : kXpRewardCatalog) {
    if (!reward.available) {
      continue;
    }
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
      {"answer_metrics_version", 2},
      {"accuracy", Percentage(summary.answers.correct_attempts, summary.answers.attempts)},
      {"attempt_accuracy", Percentage(summary.answers.correct_attempts, summary.answers.attempts)},
      {"first_answer_accuracy", Percentage(summary.answers.correct_first_answers, summary.answers.first_answers)},
      {"attempt_count", summary.answers.attempts},
      {"correct_attempt_count", summary.answers.correct_attempts},
      {"first_answer_count", summary.answers.first_answers},
      {"correct_first_answer_count", summary.answers.correct_first_answers},
      {"completion_rate", Percentage(summary.sessions_completed, summary.sessions_started)},
      {"answer_statistics_by_topic", groups(topic_answers)},
      {"answer_statistics_by_skill", groups(skill_answers)},
      {"answer_statistics_grouping", "current_question_bank"},
      {"unclassified_attempt_count", unclassified_attempts},
      {"first_try_rate", first_try_rate},
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

boost::json::object SerializeRewardAccount(const UserProgressSummary& summary, const RewardAccount& account) {
  boost::json::array catalog;
  catalog.reserve(kXpRewardCatalog.size());
  for (const auto& reward : kXpRewardCatalog) {
    const bool unlocked = account.unlocked_reward_ids.contains(std::string(reward.id));
    catalog.emplace_back(boost::json::object{
        {"id", reward.id},
        {"title", reward.title},
        {"description", reward.description},
        {"threshold", reward.threshold},
        {"remaining", std::max<std::int64_t>(0, reward.threshold - summary.xp_total)},
        {"status", !reward.available ? "planned"
                   : unlocked        ? "unlocked"
                                     : "locked"},
    });
  }

  boost::json::array newly_unlocked;
  for (const auto& id : account.newly_unlocked_reward_ids) {
    const auto* reward = FindXpReward(id);
    if (reward != nullptr) {
      newly_unlocked.emplace_back(boost::json::object{{"id", reward->id}, {"title", reward->title}});
    }
  }

  const bool frames_unlocked = account.unlocked_reward_ids.contains("profile_frames");
  boost::json::array frames{
      boost::json::object{{"id", "none"}, {"title", "Без рамки"}, {"unlocked", true}},
      boost::json::object{{"id", "cpp_blue"}, {"title", "Угловые скобки"}, {"unlocked", frames_unlocked}},
      boost::json::object{{"id", "memory_green"}, {"title", "Контур памяти"}, {"unlocked", frames_unlocked}},
  };

  boost::json::array presets;
  for (const auto& preset : account.presets) {
    boost::json::array modes;
    for (const auto& mode : preset.modes) {
      modes.emplace_back(mode);
    }
    boost::json::array topics;
    for (const auto& topic : preset.topics) {
      topics.emplace_back(topic);
    }
    presets.emplace_back(boost::json::object{
        {"slot", preset.slot},
        {"name", preset.name},
        {"modes", std::move(modes)},
        {"topics", std::move(topics)},
        {"level", preset.level},
    });
  }

  const auto confirmed = static_cast<std::int64_t>(std::ranges::count_if(summary.skills, [](const auto& skill) {
    return skill.mastery_status == SkillMasteryStatus::kDemonstrated ||
           skill.mastery_status == SkillMasteryStatus::kRetained;
  }));
  const auto retained = static_cast<std::int64_t>(std::ranges::count_if(
      summary.skills, [](const auto& skill) { return skill.mastery_status == SkillMasteryStatus::kRetained; }));
  const auto achievement = [](std::string_view id, std::string_view title, std::string_view description,
                              std::int64_t progress, std::int64_t target) {
    return boost::json::object{
        {"id", id},
        {"title", title},
        {"description", description},
        {"progress", std::min(progress, target)},
        {"target", target},
        {"completed", progress >= target},
    };
  };
  boost::json::array achievements{
      achievement("first_step", "Первый шаг", "Завершить первую карточку.", summary.completed_count, 1),
      achievement("explorer", "Исследователь", "Познакомиться с 25 разными вопросами.", summary.questions_seen, 25),
      achievement("practitioner", "Практик", "Подтвердить три навыка.", confirmed, 3),
      achievement("reliability", "Надёжность", "Закрепить навык отложенным применением.", retained, 1),
  };

  return {
      {"catalog", std::move(catalog)},
      {"newly_unlocked", std::move(newly_unlocked)},
      {"profile", boost::json::object{{"selected_frame", account.profile_frame}, {"frames", std::move(frames)}}},
      {"preset_slots", account.preset_slot_count},
      {"presets", std::move(presets)},
      {"achievements", std::move(achievements)},
  };
}

boost::json::object SerializeTrainingRecommendation(const TrainingRecommendation& recommendation) {
  return {
      {"reason", TrainingRecommendationReasonName(recommendation.reason)},
      {"title", recommendation.title},
      {"description", recommendation.description},
      {"skill_id", recommendation.skill_id},
      {"topic", recommendation.topic},
      {"topic_title", std::string(TopicTitle(recommendation.topic).value_or(recommendation.topic))},
      {"level", recommendation.level},
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

bool TheoryViewed(const MiniAppSessionState& session, std::string_view question_id) {
  return std::ranges::find(session.theory_viewed_question_ids, question_id) != session.theory_viewed_question_ids.end();
}

boost::json::object SerializeTheory(const TheoryBlock& theory) {
  boost::json::array sources;
  sources.reserve(theory.sources.size());
  for (const auto source : theory.sources)
    sources.emplace_back(source);
  return {
      {"id", theory.id},
      {"title", theory.title},
      {"principle", theory.principle},
      {"mechanism", theory.mechanism},
      {"example", theory.example},
      {"common_mistake", theory.common_mistake},
      {"sources", std::move(sources)},
  };
}

boost::json::object SerializeSessionLearning(const MiniAppSessionState& session) {
  const auto completed = session.quiz.completed_question_ids.size();
  if (session.quiz.first_try_correct > session.quiz.mastered_questions || session.quiz.mastered_questions > completed) {
    throw std::logic_error("Mini App session learning counters are inconsistent");
  }
  return {
      {"completed", completed},
      {"first_try_correct", session.quiz.first_try_correct},
      {"corrected_after_error", session.quiz.mastered_questions - session.quiz.first_try_correct},
      {"theory_opened", session.theory_viewed_question_ids.size()},
  };
}

}  // namespace

struct MiniAppService::Impl {
  using SessionState = MiniAppSessionState;

  Impl(const QuestionBank& question_bank, std::shared_ptr<DatabaseConnectionPool> connection_pool,
       std::int64_t admin_id)
      : questions(question_bank),
        quiz(question_bank),
        attempts(std::move(connection_pool)),
        feedback_admin_id(admin_id) {
    std::vector<const Question*> content;
    for (const auto& id : questions.QuestionIds()) {
      content.push_back(&questions.Get(id));
    }
    attempts.ArchiveQuestionContent(content);
  }

  const QuestionBank& questions;
  QuizSessionService quiz;
  AttemptRepository attempts;
  const std::int64_t feedback_admin_id;
  KeyedMutex user_operations;
  std::mutex sessions_mutex;
  std::unordered_map<std::int64_t, std::shared_ptr<SessionState>> sessions;

  void RestoreSession(std::int64_t user_id) {
    {
      std::scoped_lock lock(sessions_mutex);
      if (sessions.contains(user_id)) {
        return;
      }
    }
    const auto stored = attempts.LoadActiveTrainingSession(user_id);
    if (!stored) {
      return;
    }
    if (stored->content_version != kMiniAppSessionContentVersion) {
      spdlog::warn("event=session_restore outcome=incompatible");
      attempts.FinishSession(stored->session_id, "snapshot_incompatible");
      return;
    }
    try {
      auto restored = std::make_shared<SessionState>(
          ParseMiniAppSessionSnapshot(stored->state_json, stored->session_id, stored->state_version, questions));
      std::scoped_lock lock(sessions_mutex);
      sessions.emplace(user_id, std::move(restored));
    } catch (const std::exception& error) {
      spdlog::warn("event=session_restore outcome=invalid");
      attempts.FinishSession(stored->session_id, "snapshot_invalid");
    }
  }

  void PersistSession(std::int64_t user_id, SessionState& session) {
    const auto state_json = SerializeMiniAppSessionSnapshot(session);
    session.snapshot_version = attempts.UpdateActiveTrainingSession(
        user_id, session.storage_session_id, session.snapshot_version, kMiniAppSessionContentVersion, state_json);
  }

  std::shared_ptr<SessionState> RequireSession(std::int64_t user_id) {
    RestoreSession(user_id);
    std::scoped_lock lock(sessions_mutex);
    const auto it = sessions.find(user_id);
    if (it == sessions.end()) {
      throw std::logic_error("no active Mini App session");
    }
    return it->second;
  }

  std::shared_ptr<SessionState> FindSession(std::int64_t user_id) {
    std::scoped_lock lock(sessions_mutex);
    const auto it = sessions.find(user_id);
    return it == sessions.end() ? nullptr : it->second;
  }

  std::shared_ptr<SessionState> StoreSession(std::int64_t user_id, SessionState session) {
    auto stored = std::make_shared<SessionState>(std::move(session));
    std::scoped_lock lock(sessions_mutex);
    sessions.insert_or_assign(user_id, stored);
    return stored;
  }

  void EraseSession(std::int64_t user_id) {
    std::scoped_lock lock(sessions_mutex);
    sessions.erase(user_id);
  }

  void PrepareCurrent(SessionState& session) {
    const auto& question = quiz.CurrentQuestion(session.quiz);
    session.current_content_revision = question.content_revision;
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
        {"public_id", question.public_id},
        {"content_revision", question.content_revision},
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
    const bool assisted = TheoryViewed(session, question.id);
    const bool theory_available = FindTheoryForSkill(question.skill_id) != nullptr;

    std::optional<SessionState> next_session;
    std::optional<boost::json::object> next_feedback;
    const auto recorded = attempts.RecordSubmission(
        SubmissionRecord{
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
            .assisted = assisted,
            .attempt_number = submission.attempts,
            .topic_level = next_topic_state.level,
            .topic_level_evidence = next_topic_state.evidence,
            .adaptive_level = submission.session.adaptive_state.level,
            .adaptive_level_evidence = submission.session.adaptive_state.evidence,
            .content_revision = question.content_revision,
        },
        ActiveSessionUpdate{
            .expected_version = session.snapshot_version,
            .content_version = kMiniAppSessionContentVersion,
            .build_state_json =
                [&](const RecordSubmissionOutcome& outcome) {
                  SessionState candidate = session;
                  candidate.quiz = submission.session;
                  candidate.xp_gained += outcome.xp_gained;
                  candidate.snapshot_version = session.snapshot_version + 1;

                  boost::json::object feedback{
                      {"question_id", question.id},
                      {"public_id", question.public_id},
                      {"content_revision", question.content_revision},
                      {"correct", correct},
                      {"completed", submission.completed},
                      {"assisted", assisted},
                      {"attempts", submission.attempts},
                      {"answer", display_answer},
                      {"theory_available", !correct && theory_available},
                      {"theory_viewed", assisted},
                      {"previous_topic_level", previous_topic_level},
                      {"topic_level", next_topic_state.level},
                      {"previous_adaptive_level", previous_level},
                      {"adaptive_level", candidate.quiz.adaptive_state.level},
                      {"xp_gained", outcome.xp_gained},
                  };
                  if (submission.completed) {
                    feedback["explanation"] = question.explanation;
                    if (question.source)
                      feedback["source"] = *question.source;
                  }
                  if (outcome.skill_mastery) {
                    const auto& mastery = *outcome.skill_mastery;
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
                  if (!outcome.rewards_unlocked.empty()) {
                    boost::json::array rewards;
                    for (const auto& id : outcome.rewards_unlocked) {
                      const auto* reward = FindXpReward(id);
                      if (reward != nullptr) {
                        rewards.emplace_back(boost::json::object{{"id", reward->id}, {"title", reward->title}});
                      }
                    }
                    feedback["rewards_unlocked"] = std::move(rewards);
                  }

                  candidate.last_question_id = question.id;
                  candidate.last_feedback = feedback;
                  next_feedback = feedback;
                  next_session = std::move(candidate);
                  return SerializeMiniAppSessionSnapshot(*next_session);
                },
        });

    if (recorded.result == RecordSubmissionResult::kDuplicate) {
      const auto stored = attempts.LoadActiveTrainingSession(user_id);
      if (!stored || stored->content_version != kMiniAppSessionContentVersion) {
        throw std::logic_error("cannot recover the already recorded Mini App answer");
      }
      session = ParseMiniAppSessionSnapshot(stored->state_json, stored->session_id, stored->state_version, questions);
      if (!session.last_feedback) {
        throw std::logic_error("restored Mini App answer has no feedback");
      }
      return *session.last_feedback;
    }
    if (!next_session || !next_feedback) {
      throw std::logic_error("active Mini App session update was not built");
    }
    session = std::move(*next_session);
    return std::move(*next_feedback);
  }
};

MiniAppService::MiniAppService(const QuestionBank& questions, std::string database_url, std::int64_t feedback_admin_id)
    : MiniAppService(questions, std::make_shared<DatabaseConnectionPool>(std::move(database_url), 1),
                     feedback_admin_id) {}

MiniAppService::MiniAppService(const QuestionBank& questions, std::shared_ptr<DatabaseConnectionPool> connection_pool,
                               std::int64_t feedback_admin_id)
    : impl_(std::make_unique<Impl>(questions, std::move(connection_pool), feedback_admin_id)) {}

MiniAppService::~MiniAppService() = default;

namespace {

boost::json::object FeedbackReceipt(const CardFeedbackRecord& record) {
  return {{"feedback_id", record.id},
          {"public_id", record.submission.public_id},
          {"content_revision", record.submission.content_revision},
          {"status", record.status}};
}

std::int64_t FeedbackRequestNumber(const boost::json::object& request, std::string_view key) {
  const auto number = RequireSize(request, key);
  if (number > static_cast<std::size_t>(std::numeric_limits<std::int64_t>::max())) {
    throw std::invalid_argument("feedback identifier is too large");
  }
  return static_cast<std::int64_t>(number);
}

std::string RequireUuid(const boost::json::object& request, std::string_view key) {
  auto value = RequireString(request, key, 36);
  if (value.size() != 36) {
    throw std::invalid_argument("request identifier must be a UUID");
  }
  for (std::size_t i = 0; i < value.size(); ++i) {
    const char ch = value[i];
    const bool separator = i == 8 || i == 13 || i == 18 || i == 23;
    if ((separator && ch != '-') || (!separator && !((ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f')))) {
      throw std::invalid_argument("request identifier must be a lowercase UUID");
    }
  }
  return value;
}

}  // namespace

bool MiniAppService::CheckReady() {
  return impl_->questions.Size() > 0 && impl_->attempts.CheckReady();
}

boost::json::object MiniAppService::Statistics(std::int64_t user_id, const boost::json::object& request) {
  if (!IsFeedbackAdmin(user_id)) {
    throw FeedbackAccessDenied("Статистика доступна только владельцу приложения.");
  }
  const auto days = RequireSize(request, "days");
  if (days < 1 || days > 90) {
    throw std::invalid_argument("Выбери период от 1 до 90 дней.");
  }
  return boost::json::parse(impl_->attempts.BetaStatistics(static_cast<int>(days))).as_object();
}

boost::json::object MiniAppService::RecordActivity(std::int64_t user_id, const boost::json::object& request) {
  [[maybe_unused]] auto user_operation = impl_->user_operations.Acquire(user_id);
  const auto visit = RequireUuid(request, "visit_id");
  const auto sequence = FeedbackRequestNumber(request, "sequence");
  if (sequence > 1'000'000'000) {
    throw std::invalid_argument("activity sequence is too large");
  }
  std::string context = "home";
  std::string question_id;
  std::string kind;
  std::int64_t session_id = 0;
  const bool visible = OptionalBoolean(request, "visible");
  const auto requested_context = RequireString(request, "context", 16);
  if (!visible) {
    context = "hidden";
  } else if (requested_context == "admin" && IsFeedbackAdmin(user_id)) {
    context = "admin";
  } else if (requested_context == "report") {
    context = "report";
  } else {
    impl_->RestoreSession(user_id);
    if (const auto session = impl_->FindSession(user_id)) {
      session_id = session->storage_session_id;
      const auto& question = session->quiz.answered ? impl_->questions.Get(*session->last_question_id)
                                                    : impl_->quiz.CurrentQuestion(session->quiz);
      question_id = question.id;
      context = session->quiz.answered ? "feedback" : "question";
      if (context == "feedback" && requested_context == "theory" && TheoryViewed(*session, question_id)) {
        context = "theory";
      }
      const auto review = session->quiz.review_states.find(question_id);
      kind = session->current_attempt > 1                  ? "repair"
             : review == session->quiz.review_states.end() ? "new"
             : review->second.due                          ? "review"
                                                           : "early_review";
    }
  }
  return {
      {"accepted", impl_->attempts.RecordActivity(user_id, visit, sequence, session_id, question_id, context, kind)}};
}

bool MiniAppService::IsFeedbackAdmin(std::int64_t user_id) const noexcept {
  return impl_->feedback_admin_id > 0 && user_id == impl_->feedback_admin_id;
}

boost::json::object MiniAppService::SubmitCardFeedback(std::int64_t user_id, const boost::json::object& request) {
  [[maybe_unused]] auto user_operation = impl_->user_operations.Acquire(user_id);
  const auto request_id = RequireString(request, "request_id", 36);
  if (request_id.size() != 36) {
    throw std::invalid_argument("feedback request_id must be a UUID");
  }
  for (std::size_t i = 0; i < request_id.size(); ++i) {
    const bool separator = i == 8 || i == 13 || i == 18 || i == 23;
    const char ch = request_id[i];
    if ((separator && ch != '-') || (!separator && !((ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f')))) {
      throw std::invalid_argument("feedback request_id must be a lowercase UUID");
    }
  }
  const auto question_id = RequireString(request, "question_id", 256);
  const auto revision = RequireString(request, "content_revision", 64);
  const auto phase = RequireString(request, "phase", 16);
  const auto category = RequireString(request, "category", 32);
  const auto comment = RequireString(request, "comment", 8000);
  // A lost response can be retried even after the session has finished or the bank has changed.
  if (const auto existing = impl_->attempts.FindCardFeedback(user_id, request_id)) {
    const auto& first = existing->submission;
    if (first.question_id != question_id || first.content_revision != revision || first.phase != phase ||
        first.category != category || first.comment != comment) {
      throw std::logic_error("Этот номер запроса уже использован для другого отзыва.");
    }
    return FeedbackReceipt(*existing);
  }
  const auto session = impl_->RequireSession(user_id);
  const auto expected_phase = session->quiz.answered ? "feedback" : "question";
  const auto presented_id =
      session->quiz.answered ? *session->last_question_id : impl_->quiz.CurrentQuestion(session->quiz).id;
  if (phase != expected_phase || question_id != presented_id) {
    throw std::logic_error("Карточка уже сменилась. Открой форму отзыва на текущей карточке.");
  }
  const auto snapshot = impl_->attempts.LoadQuestionContent(question_id, revision);
  if (!snapshot) {
    throw std::logic_error("Эта редакция карточки недоступна. Обнови приложение.");
  }
  const auto content = boost::json::parse(*snapshot).as_object();
  const auto record = impl_->attempts.SaveCardFeedback({
      .user_id = user_id,
      .session_id = session->storage_session_id,
      .request_id = request_id,
      .question_id = question_id,
      .public_id = CopyString(content.at("public_id").as_string()),
      .content_revision = revision,
      .content_snapshot_json = *snapshot,
      .phase = phase,
      .category = category,
      .comment = comment,
  });
  return FeedbackReceipt(record);
}

boost::json::object MiniAppService::ListCardFeedback(std::int64_t user_id, const boost::json::object& request) {
  if (!IsFeedbackAdmin(user_id)) {
    throw FeedbackAccessDenied("Очередь отзывов доступна только владельцу приложения.");
  }
  const auto requested_status = RequireString(request, "status", 16);
  const auto before_id = request.if_contains("before_id") ? FeedbackRequestNumber(request, "before_id") : 0;
  const auto records = impl_->attempts.ListCardFeedback(requested_status == "all" ? "" : requested_status, before_id);
  boost::json::array items;
  for (const auto& record : records) {
    auto item = FeedbackReceipt(record);
    item["question_id"] = record.submission.question_id;
    item["phase"] = record.submission.phase;
    item["category"] = record.submission.category;
    item["comment"] = record.submission.comment;
    item["content"] = boost::json::parse(record.submission.content_snapshot_json);
    item["decision"] = record.decision;
    item["source"] = record.source;
    item["version"] = record.version;
    item["created_at"] = record.created_at;
    item["updated_at"] = record.updated_at;
    items.emplace_back(std::move(item));
  }
  return {{"items", std::move(items)}, {"next_before_id", records.size() == 30 ? records.back().id : 0}};
}

boost::json::object MiniAppService::ReviewCardFeedback(std::int64_t user_id, const boost::json::object& request) {
  if (!IsFeedbackAdmin(user_id)) {
    throw FeedbackAccessDenied("Очередь отзывов доступна только владельцу приложения.");
  }
  const auto status = RequireString(request, "status", 16);
  const auto optional_text = [&](std::string_view key, std::size_t limit) {
    const auto* value = request.if_contains(key);
    if (value == nullptr || !value->is_string()) {
      throw std::invalid_argument(std::string(key) + " must be a string");
    }
    auto text = CopyString(value->as_string());
    if (text.size() > limit || text.find('\0') != std::string::npos) {
      throw std::invalid_argument("feedback review text is too long or contains invalid characters");
    }
    return text;
  };
  const auto record = impl_->attempts.ReviewCardFeedback(
      FeedbackRequestNumber(request, "feedback_id"), FeedbackRequestNumber(request, "version"), user_id, status,
      optional_text("decision", 8000), optional_text("source", 2048));
  auto result = FeedbackReceipt(record);
  result["version"] = record.version;
  return result;
}

boost::json::object MiniAppService::Bootstrap(const TelegramMiniAppUser& user) {
  [[maybe_unused]] auto user_operation = impl_->user_operations.Acquire(user.id);
  impl_->attempts.EnsureUser(user.id);
  impl_->RestoreSession(user.id);
  const auto summary = impl_->attempts.LoadProgressSummary(user.id);
  const auto reward_account = impl_->attempts.LoadRewardAccount(user.id);
  const auto review_states = impl_->attempts.LoadReviewStates(user.id);
  const auto clean_question_families = impl_->attempts.LoadCleanQuestionFamilies(user.id);
  const auto recommendation = RecommendTraining(impl_->questions, summary, review_states, clean_question_families);
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
        {"assessable", IsMasteryPilotSkill(skill.id)},
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
      {"recommendation", SerializeTrainingRecommendation(recommendation)},
      {"summary", SerializeProgressSummary(summary, impl_->questions)},
      {"rewards", SerializeRewardAccount(summary, reward_account)},
      {"feedback_admin", IsFeedbackAdmin(user.id)},
  };
  const auto session = impl_->FindSession(user.id);
  result["session"] =
      session == nullptr ? boost::json::value(nullptr) : boost::json::value(impl_->SerializeSession(*session));
  return result;
}

boost::json::object MiniAppService::StartSession(std::int64_t user_id, const boost::json::object& request) {
  [[maybe_unused]] auto user_operation = impl_->user_operations.Acquire(user_id);
  const auto request_id = request.if_contains("request_id") ? RequireUuid(request, "request_id") : std::string{};
  if (!request_id.empty()) {
    if (const auto previous = impl_->attempts.FindSessionByRequest(user_id, request_id)) {
      const auto active = impl_->attempts.LoadActiveTrainingSession(user_id);
      if (!active || active->session_id != *previous) {
        throw std::logic_error("Эта тренировка уже завершена. Обнови главный экран.");
      }
      const auto restored = impl_->StoreSession(
          user_id,
          ParseMiniAppSessionSnapshot(active->state_json, active->session_id, active->state_version, impl_->questions));
      return impl_->SerializeSession(*restored);
    }
  }
  const bool recommended = OptionalBoolean(request, "recommended");
  const auto unlocked_level = impl_->attempts.GetUnlockedLevel(user_id);
  auto review_states = impl_->attempts.LoadReviewStates(user_id);

  std::vector<TrainingContentMode> modes;
  std::vector<std::string> topics;
  int selected_level = 1;
  QuestionSelection selection;
  std::optional<TrainingRecommendation> recommendation;

  if (recommended) {
    const auto summary = impl_->attempts.LoadProgressSummary(user_id);
    const auto clean_question_families = impl_->attempts.LoadCleanQuestionFamilies(user_id);
    recommendation = RecommendTraining(impl_->questions, summary, review_states, clean_question_families);
    modes = {TrainingContentMode::kTheory, TrainingContentMode::kApi, TrainingContentMode::kCode};
    topics = {recommendation->topic};
    selected_level = recommendation->level;
    selection = QuestionSelectionForModes(modes, topics);
    selection.preferred_question_ids = {recommendation->question_id};
  } else {
    modes = ParseModes(request);
    topics = ParseTopics(request);
    selected_level = RequireLevel(request, "level");
    selection = QuestionSelectionForModes(modes, topics);
  }

  if (selected_level > unlocked_level) {
    throw std::invalid_argument("этот уровень ещё не открыт");
  }
  if (!recommended && LevelQuestionCount(impl_->questions, selected_level) < kMinimumQuestionsForSelectableLevel) {
    throw std::invalid_argument("для этого уровня пока недостаточно вопросов");
  }
  selection.min_level = selected_level;
  selection.max_level = selected_level;
  if (!impl_->quiz.HasQuestions(selection)) {
    throw std::invalid_argument("на выбранном уровне нет вопросов для этих тем и форматов");
  }

  auto adaptive_state = impl_->attempts.GetAdaptiveState(user_id);
  auto topic_states = impl_->attempts.LoadTopicStates(user_id);
  auto quiz = impl_->quiz.Start(selection, adaptive_state, std::move(topic_states), std::move(review_states));
  std::string storage_mode =
      recommended ? "miniapp:recommended:" + std::string(TrainingRecommendationReasonName(recommendation->reason)) +
                        ":level-" + std::to_string(selected_level)
                  : "miniapp:" + TrainingModeStorageName(modes) + ":level-" + std::to_string(selected_level);
  Impl::SessionState state{
      .quiz = std::move(quiz),
      .storage_session_id = 0,
      .snapshot_version = 0,
      .modes = std::move(modes),
      .topics = std::move(topics),
      .selected_level = selected_level,
      .tracks_level_progress = IsProgressionPractice(selected_level, unlocked_level),
      .option_order = {},
      .matching = std::nullopt,
      .questions_started = 0,
      .current_attempt = 1,
      .xp_gained = 0,
      .theory_viewed_question_ids = {},
      .last_question_id = std::nullopt,
      .last_feedback = std::nullopt,
  };
  impl_->PrepareCurrent(state);
  const auto initial_snapshot = SerializeMiniAppSessionSnapshot(state);
  state.storage_session_id = impl_->attempts.StartActiveTrainingSession(
      user_id, storage_mode, kMiniAppSessionContentVersion, initial_snapshot, request_id);
  if (!request_id.empty()) {
    const auto active = impl_->attempts.LoadActiveTrainingSession(user_id);
    if (!active || active->session_id != state.storage_session_id) {
      throw std::logic_error("Не удалось восстановить начатую тренировку. Обнови главный экран.");
    }
    const auto restored = impl_->StoreSession(
        user_id,
        ParseMiniAppSessionSnapshot(active->state_json, active->session_id, active->state_version, impl_->questions));
    return impl_->SerializeSession(*restored);
  }
  state.snapshot_version = 1;
  const auto stored = impl_->StoreSession(user_id, std::move(state));
  return impl_->SerializeSession(*stored);
}

boost::json::object MiniAppService::CurrentSession(std::int64_t user_id) {
  [[maybe_unused]] auto user_operation = impl_->user_operations.Acquire(user_id);
  return impl_->SerializeSession(*impl_->RequireSession(user_id));
}

boost::json::object MiniAppService::SubmitAnswer(std::int64_t user_id, const boost::json::object& request) {
  [[maybe_unused]] auto user_operation = impl_->user_operations.Acquire(user_id);
  const auto stored = impl_->RequireSession(user_id);
  auto& session = *stored;
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
    boost::json::array stored_answers;
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
      stored_answers.emplace_back(answer);
    submission.emplace(impl_->quiz.SubmitAnswer(session.quiz, std::span<const std::string>(answers)));
    storage_answer = boost::json::serialize(stored_answers);
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
  [[maybe_unused]] auto user_operation = impl_->user_operations.Acquire(user_id);
  const auto stored = impl_->RequireSession(user_id);
  auto& session = *stored;
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
  auto next = session;
  const auto outcome = next.matching->Select(side, RequireSize(request, "position"), RequireSize(request, "revision"));
  if (outcome == MatchingPick::kFinished) {
    const auto mistakes = next.matching->Mistakes();
    const auto storage_answer = boost::json::serialize(boost::json::object{
        {"source", "mini_app_api"},
        {"matched", next.matching->MatchedCount()},
        {"mistakes", mistakes},
    });
    const auto display_answer = std::to_string(task->pairs.size()) + "/" + std::to_string(task->pairs.size()) +
                                " пар, ошибок: " + std::to_string(mistakes);
    impl_->ApplySubmission(user_id, next, question, storage_answer, display_answer,
                           impl_->quiz.SubmitMatchingAnswer(next.quiz, mistakes == 0));
  } else if (outcome != MatchingPick::kIgnored) {
    impl_->PersistSession(user_id, next);
  }
  session = std::move(next);

  return {
      {"outcome", MatchingOutcomeName(outcome)},
      {"session", impl_->SerializeSession(session)},
  };
}

boost::json::object MiniAppService::OpenTheory(std::int64_t user_id, const boost::json::object& request) {
  [[maybe_unused]] auto user_operation = impl_->user_operations.Acquire(user_id);
  const auto stored = impl_->RequireSession(user_id);
  auto& session = *stored;
  const auto question_id = RequireString(request, "question_id", 256);

  if (!session.quiz.answered || !session.last_question_id || !session.last_feedback ||
      *session.last_question_id != question_id) {
    throw std::logic_error("теория доступна только после ответа на текущую карточку");
  }
  const auto* correct = session.last_feedback->if_contains("correct");
  if (correct == nullptr || !correct->is_bool() || correct->as_bool()) {
    throw std::logic_error("разбор теории открывается после ошибки");
  }

  const auto& question = impl_->quiz.GetQuestion(question_id);
  const auto* theory = FindTheoryForSkill(question.skill_id);
  if (theory == nullptr) {
    throw std::invalid_argument("для этого навыка теоретический разбор ещё не готов");
  }

  if (!TheoryViewed(session, question_id)) {
    auto next = session;
    next.theory_viewed_question_ids.push_back(question_id);
    (*next.last_feedback)["theory_viewed"] = true;
    impl_->PersistSession(user_id, next);
    session = std::move(next);
  }

  return {
      {"theory", SerializeTheory(*theory)},
      {"session", impl_->SerializeSession(session)},
  };
}

boost::json::object MiniAppService::NextQuestion(std::int64_t user_id) {
  [[maybe_unused]] auto user_operation = impl_->user_operations.Acquire(user_id);
  const auto stored = impl_->RequireSession(user_id);
  auto& session = *stored;
  if (!session.quiz.answered) {
    throw std::logic_error("answer the current question first");
  }

  auto next = session;
  next.quiz = impl_->quiz.Refill(next.quiz);
  if (next.quiz.queue.Empty()) {
    const auto storage_session_id = session.storage_session_id;
    const auto xp_gained = session.xp_gained;
    const auto learning = SerializeSessionLearning(session);
    impl_->attempts.FinishSession(storage_session_id, "completed");
    impl_->EraseSession(user_id);
    return {
        {"status", "completed"},
        {"xp_gained", xp_gained},
        {"learning", learning},
        {"summary", SerializeProgressSummary(impl_->attempts.LoadProgressSummary(user_id), impl_->questions)},
    };
  }

  impl_->PrepareCurrent(next);
  impl_->PersistSession(user_id, next);
  session = std::move(next);
  return impl_->SerializeSession(session);
}

boost::json::object MiniAppService::StopSession(std::int64_t user_id) {
  [[maybe_unused]] auto user_operation = impl_->user_operations.Acquire(user_id);
  impl_->RestoreSession(user_id);
  if (const auto session = impl_->FindSession(user_id)) {
    impl_->attempts.FinishSession(session->storage_session_id, "miniapp_stopped");
    impl_->EraseSession(user_id);
  }
  return {
      {"status", "stopped"},
      {"summary", SerializeProgressSummary(impl_->attempts.LoadProgressSummary(user_id), impl_->questions)},
  };
}

boost::json::object MiniAppService::SetProfileFrame(std::int64_t user_id, const boost::json::object& request) {
  [[maybe_unused]] auto user_operation = impl_->user_operations.Acquire(user_id);
  const auto frame_id = RequireString(request, "frame_id", 32);
  static constexpr std::array<std::string_view, 3> kFrames{"none", "cpp_blue", "memory_green"};
  if (std::ranges::find(kFrames, frame_id) == kFrames.end()) {
    throw std::invalid_argument("неизвестная рамка профиля");
  }
  impl_->attempts.SetProfileFrame(user_id, frame_id);
  const auto summary = impl_->attempts.LoadProgressSummary(user_id);
  return {{"rewards", SerializeRewardAccount(summary, impl_->attempts.LoadRewardAccount(user_id))}};
}

boost::json::object MiniAppService::SaveTrainingPreset(std::int64_t user_id, const boost::json::object& request) {
  [[maybe_unused]] auto user_operation = impl_->user_operations.Acquire(user_id);
  const auto slot_value = RequireSize(request, "slot");
  if (slot_value < 1 || slot_value > 2) {
    throw std::invalid_argument("номер набора должен быть 1 или 2");
  }
  const auto modes = ParseModes(request);
  std::vector<std::string> mode_names;
  mode_names.reserve(modes.size());
  for (const auto mode : modes) {
    mode_names.push_back(TrainingModeName(mode));
  }
  auto topics = ParseTopics(request);
  const auto level = RequireLevel(request, "level");
  if (level > impl_->attempts.GetUnlockedLevel(user_id)) {
    throw std::invalid_argument("нельзя сохранить ещё не открытую сложность");
  }
  impl_->attempts.SaveTrainingPreset(user_id, TrainingPreset{
                                                  .slot = static_cast<int>(slot_value),
                                                  .name = RequireString(request, "name", 80),
                                                  .modes = std::move(mode_names),
                                                  .topics = std::move(topics),
                                                  .level = level,
                                              });
  const auto summary = impl_->attempts.LoadProgressSummary(user_id);
  return {{"rewards", SerializeRewardAccount(summary, impl_->attempts.LoadRewardAccount(user_id))}};
}
