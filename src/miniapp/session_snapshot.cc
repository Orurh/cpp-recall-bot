#include "session_snapshot.h"

#include "app/training_mode.h"
#include "learning/adaptive_level.h"
#include "learning/learning_queue.h"
#include "learning/review_state.h"
#include "quiz/matching_round.h"
#include "quiz/question.h"
#include "quiz/question_bank.h"
#include "quiz/session.h"

#include <boost/json.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <ranges>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <variant>
#include <vector>

namespace {

constexpr std::size_t kMaximumSnapshotItems = 1'000;

std::string CopyString(const boost::json::string& value) {
  return {value.data(), value.size()};
}

const boost::json::value& RequireValue(const boost::json::object& object, std::string_view key) {
  const auto* value = object.if_contains(key);
  if (value == nullptr) {
    throw std::invalid_argument("session snapshot field is missing: " + std::string(key));
  }
  return *value;
}

const boost::json::object& RequireObject(const boost::json::object& object, std::string_view key) {
  const auto& value = RequireValue(object, key);
  if (!value.is_object()) {
    throw std::invalid_argument("session snapshot field must be an object: " + std::string(key));
  }
  return value.as_object();
}

const boost::json::array& RequireArray(const boost::json::object& object, std::string_view key) {
  const auto& value = RequireValue(object, key);
  if (!value.is_array() || value.as_array().size() > kMaximumSnapshotItems) {
    throw std::invalid_argument("session snapshot field must be a bounded array: " + std::string(key));
  }
  return value.as_array();
}

std::string RequireString(const boost::json::object& object, std::string_view key, std::size_t maximum = 4096) {
  const auto& value = RequireValue(object, key);
  if (!value.is_string()) {
    throw std::invalid_argument("session snapshot field must be a string: " + std::string(key));
  }
  auto result = CopyString(value.as_string());
  if (result.empty() || result.size() > maximum) {
    throw std::invalid_argument("session snapshot string has invalid length: " + std::string(key));
  }
  return result;
}

std::int64_t RequireInt64(const boost::json::object& object, std::string_view key) {
  const auto& value = RequireValue(object, key);
  if (value.is_int64()) {
    return value.as_int64();
  }
  if (value.is_uint64() && value.as_uint64() <= static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
    return static_cast<std::int64_t>(value.as_uint64());
  }
  throw std::invalid_argument("session snapshot field must be an integer: " + std::string(key));
}

std::size_t RequireSize(const boost::json::object& object, std::string_view key,
                        std::size_t maximum = kMaximumSnapshotItems) {
  const auto value = RequireInt64(object, key);
  if (value < 0 || static_cast<std::uint64_t>(value) > maximum) {
    throw std::invalid_argument("session snapshot size is outside its allowed range: " + std::string(key));
  }
  return static_cast<std::size_t>(value);
}

int RequireInt(const boost::json::object& object, std::string_view key, int minimum, int maximum) {
  const auto value = RequireInt64(object, key);
  if (value < minimum || value > maximum) {
    throw std::invalid_argument("session snapshot integer is outside its allowed range: " + std::string(key));
  }
  return static_cast<int>(value);
}

bool RequireBool(const boost::json::object& object, std::string_view key) {
  const auto& value = RequireValue(object, key);
  if (!value.is_bool()) {
    throw std::invalid_argument("session snapshot field must be a boolean: " + std::string(key));
  }
  return value.as_bool();
}

boost::json::array StringsToJson(const std::vector<std::string>& values) {
  boost::json::array result;
  result.reserve(values.size());
  for (const auto& value : values) {
    result.emplace_back(value);
  }
  return result;
}

std::vector<std::string> ParseStrings(const boost::json::array& values, std::string_view field,
                                      bool allow_empty = true) {
  std::vector<std::string> result;
  std::unordered_set<std::string> unique;
  result.reserve(values.size());
  for (const auto& value : values) {
    if (!value.is_string()) {
      throw std::invalid_argument("session snapshot array must contain strings: " + std::string(field));
    }
    auto item = CopyString(value.as_string());
    if (item.empty() || item.size() > 256 || !unique.insert(item).second) {
      throw std::invalid_argument("session snapshot contains an invalid or duplicate value: " + std::string(field));
    }
    result.push_back(std::move(item));
  }
  if (!allow_empty && result.empty()) {
    throw std::invalid_argument("session snapshot array must not be empty: " + std::string(field));
  }
  return result;
}

std::string_view TrainingModeName(TrainingContentMode mode) {
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

TrainingContentMode ParseTrainingMode(std::string_view mode) {
  if (mode == "theory")
    return TrainingContentMode::kTheory;
  if (mode == "api")
    return TrainingContentMode::kApi;
  if (mode == "code")
    return TrainingContentMode::kCode;
  if (mode == "matching")
    return TrainingContentMode::kMatching;
  throw std::invalid_argument("session snapshot contains an unknown training mode");
}

std::string_view QuestionKindName(QuestionKind kind) {
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

QuestionKind ParseQuestionKind(std::string_view kind) {
  if (kind == "theory")
    return QuestionKind::kTheory;
  if (kind == "api_knowledge")
    return QuestionKind::kApiKnowledge;
  if (kind == "code_reading")
    return QuestionKind::kCodeReading;
  if (kind == "debugging")
    return QuestionKind::kDebugging;
  if (kind == "implementation")
    return QuestionKind::kImplementation;
  throw std::invalid_argument("session snapshot contains an unknown question kind");
}

boost::json::object AdaptiveStateToJson(const AdaptiveLevelState& state) {
  return {{"level", state.level}, {"evidence", state.evidence}};
}

AdaptiveLevelState ParseAdaptiveState(const boost::json::object& object) {
  AdaptiveLevelState state{
      .level = RequireInt(object, "level", 1, 10),
      .evidence = RequireInt(object, "evidence", -3, 3),
  };
  ValidateAdaptiveLevelState(state);
  return state;
}

boost::json::object SelectionToJson(const QuestionSelection& selection) {
  boost::json::array kinds;
  kinds.reserve(selection.kinds.size());
  for (const auto kind : selection.kinds) {
    kinds.emplace_back(QuestionKindName(kind));
  }
  return {
      {"topics", StringsToJson(selection.topics)},
      {"skill_ids", StringsToJson(selection.skill_ids)},
      {"kinds", std::move(kinds)},
      {"matching_only", selection.matching_only},
      {"min_level", selection.min_level},
      {"max_level", selection.max_level},
      {"preferred_question_ids", StringsToJson(selection.preferred_question_ids)},
  };
}

QuestionSelection ParseSelection(const boost::json::object& object) {
  QuestionSelection selection{
      .topics = ParseStrings(RequireArray(object, "topics"), "topics"),
      .skill_ids = ParseStrings(RequireArray(object, "skill_ids"), "skill_ids"),
      .kinds = {},
      .matching_only = RequireBool(object, "matching_only"),
      .min_level = RequireInt(object, "min_level", 1, 10),
      .max_level = RequireInt(object, "max_level", 1, 10),
      .preferred_question_ids = ParseStrings(RequireArray(object, "preferred_question_ids"), "preferred_question_ids"),
  };
  if (selection.min_level > selection.max_level) {
    throw std::invalid_argument("session snapshot has an inverted question level range");
  }
  for (const auto& value : RequireArray(object, "kinds")) {
    if (!value.is_string()) {
      throw std::invalid_argument("session snapshot question kind must be a string");
    }
    const auto kind = ParseQuestionKind(CopyString(value.as_string()));
    if (std::ranges::find(selection.kinds, kind) != selection.kinds.end()) {
      throw std::invalid_argument("session snapshot contains a duplicate question kind");
    }
    selection.kinds.push_back(kind);
  }
  return selection;
}

boost::json::object QuizToJson(const QuizSession& quiz) {
  const auto serialize_queue = [](const auto& entries) {
    boost::json::array result;
    result.reserve(entries.size());
    for (const auto& entry : entries) {
      result.emplace_back(boost::json::object{{"question_id", entry.question_id}, {"attempts", entry.attempts}});
    }
    return result;
  };

  boost::json::array topic_states;
  topic_states.reserve(quiz.topic_states.size());
  for (const auto& [topic, state] : quiz.topic_states) {
    topic_states.emplace_back(boost::json::object{
        {"topic", topic},
        {"state", AdaptiveStateToJson(state)},
    });
  }

  boost::json::array review_states;
  review_states.reserve(quiz.review_states.size());
  for (const auto& [question_id, state] : quiz.review_states) {
    review_states.emplace_back(boost::json::object{
        {"question_id", question_id},
        {"due", state.due},
        {"stage", state.stage},
        {"seconds_until_due", state.seconds_until_due},
        {"correct_count", state.correct_count},
        {"failed_count", state.failed_count},
    });
  }

  return {
      {"primary_queue", serialize_queue(quiz.queue.PrimaryPending())},
      {"repair_queue", serialize_queue(quiz.queue.RepairPending())},
      {"selection", SelectionToJson(quiz.selection)},
      {"completed_question_ids", StringsToJson(quiz.completed_question_ids)},
      {"topic_states", std::move(topic_states)},
      {"review_states", std::move(review_states)},
      {"session_goal", quiz.session_goal},
      {"total_attempts", quiz.total_attempts},
      {"correct_attempts", quiz.correct_attempts},
      {"mastered_questions", quiz.mastered_questions},
      {"first_try_correct", quiz.first_try_correct},
      {"current_streak", quiz.current_streak},
      {"best_streak", quiz.best_streak},
      {"initial_level", quiz.initial_level},
      {"adaptive_state", AdaptiveStateToJson(quiz.adaptive_state)},
      {"answered", quiz.answered},
  };
}

QuizSession ParseQuiz(const boost::json::object& object, const QuestionBank& questions) {
  const auto parse_queue = [&object, &questions](std::string_view field) {
    std::vector<LearningQueue::Entry> result;
    result.reserve(RequireArray(object, field).size());
    for (const auto& value : RequireArray(object, field)) {
      if (!value.is_object()) {
        throw std::invalid_argument("session snapshot queue entry must be an object");
      }
      const auto& entry = value.as_object();
      auto question_id = RequireString(entry, "question_id", 256);
      static_cast<void>(questions.Get(question_id));
      result.push_back({
          .question_id = std::move(question_id),
          .attempts = RequireSize(entry, "attempts", 1'000),
      });
    }
    return result;
  };
  auto primary_queue = parse_queue("primary_queue");
  auto repair_queue = parse_queue("repair_queue");

  std::unordered_map<std::string, AdaptiveLevelState> topic_states;
  for (const auto& value : RequireArray(object, "topic_states")) {
    if (!value.is_object()) {
      throw std::invalid_argument("session snapshot topic state must be an object");
    }
    const auto& item = value.as_object();
    if (!topic_states.emplace(RequireString(item, "topic", 256), ParseAdaptiveState(RequireObject(item, "state")))
             .second) {
      throw std::invalid_argument("session snapshot contains a duplicate topic state");
    }
  }

  std::unordered_map<std::string, QuestionReviewState> review_states;
  for (const auto& value : RequireArray(object, "review_states")) {
    if (!value.is_object()) {
      throw std::invalid_argument("session snapshot review state must be an object");
    }
    const auto& item = value.as_object();
    QuestionReviewState state{
        .due = RequireBool(item, "due"),
        .stage = RequireInt(item, "stage", 0, 5),
        .seconds_until_due = RequireInt64(item, "seconds_until_due"),
        .correct_count = RequireInt64(item, "correct_count"),
        .failed_count = RequireInt64(item, "failed_count"),
    };
    if (state.correct_count < 0 || state.failed_count < 0) {
      throw std::invalid_argument("session snapshot review counters must not be negative");
    }
    if (!review_states.emplace(RequireString(item, "question_id", 256), state).second) {
      throw std::invalid_argument("session snapshot contains a duplicate review state");
    }
  }

  return {
      .queue = LearningQueue(std::move(primary_queue), std::move(repair_queue), LearningQueue::RestoreStateTag{}),
      .selection = ParseSelection(RequireObject(object, "selection")),
      .completed_question_ids = ParseStrings(RequireArray(object, "completed_question_ids"), "completed_question_ids"),
      .topic_states = std::move(topic_states),
      .review_states = std::move(review_states),
      .session_goal = RequireSize(object, "session_goal", 100),
      .total_attempts = RequireSize(object, "total_attempts", 10'000),
      .correct_attempts = RequireSize(object, "correct_attempts", 10'000),
      .mastered_questions = RequireSize(object, "mastered_questions", 10'000),
      .first_try_correct = RequireSize(object, "first_try_correct", 10'000),
      .current_streak = RequireSize(object, "current_streak", 10'000),
      .best_streak = RequireSize(object, "best_streak", 10'000),
      .initial_level = RequireInt(object, "initial_level", 1, 10),
      .adaptive_state = ParseAdaptiveState(RequireObject(object, "adaptive_state")),
      .answered = RequireBool(object, "answered"),
  };
}

boost::json::object MatchingToJson(const MatchingRound& round) {
  const auto state = round.State();
  boost::json::array left_order;
  boost::json::array right_order;
  boost::json::array matched;
  left_order.reserve(state.left_order.size());
  right_order.reserve(state.right_order.size());
  matched.reserve(state.matched.size());
  for (const auto index : state.left_order)
    left_order.emplace_back(index);
  for (const auto index : state.right_order)
    right_order.emplace_back(index);
  for (const auto value : state.matched)
    matched.emplace_back(value);

  return {
      {"left_order", std::move(left_order)},
      {"right_order", std::move(right_order)},
      {"matched", std::move(matched)},
      {"selected_left", state.selected_left ? boost::json::value(*state.selected_left) : boost::json::value(nullptr)},
      {"selected_right",
       state.selected_right ? boost::json::value(*state.selected_right) : boost::json::value(nullptr)},
      {"mistakes", state.mistakes},
      {"revision", state.revision},
      {"last_mismatch", state.last_mismatch},
  };
}

std::vector<std::size_t> ParseIndices(const boost::json::array& values, std::string_view field) {
  std::vector<std::size_t> result;
  result.reserve(values.size());
  for (const auto& value : values) {
    if (!value.is_int64() && !value.is_uint64()) {
      throw std::invalid_argument("session snapshot matching index must be an integer: " + std::string(field));
    }
    const auto index = value.is_uint64() ? value.as_uint64() : static_cast<std::uint64_t>(value.as_int64());
    if ((value.is_int64() && value.as_int64() < 0) || index > 6) {
      throw std::invalid_argument("session snapshot matching index is outside its allowed range");
    }
    result.push_back(static_cast<std::size_t>(index));
  }
  return result;
}

std::optional<std::size_t> ParseOptionalIndex(const boost::json::object& object, std::string_view key) {
  const auto& value = RequireValue(object, key);
  if (value.is_null())
    return std::nullopt;
  boost::json::object wrapped{{"value", value}};
  return RequireSize(wrapped, "value", 6);
}

MatchingRound ParseMatching(const boost::json::object& object) {
  std::vector<bool> matched;
  for (const auto& value : RequireArray(object, "matched")) {
    if (!value.is_bool()) {
      throw std::invalid_argument("session snapshot matching flags must be boolean");
    }
    matched.push_back(value.as_bool());
  }
  return MatchingRound(MatchingRoundState{
      .left_order = ParseIndices(RequireArray(object, "left_order"), "left_order"),
      .right_order = ParseIndices(RequireArray(object, "right_order"), "right_order"),
      .matched = std::move(matched),
      .selected_left = ParseOptionalIndex(object, "selected_left"),
      .selected_right = ParseOptionalIndex(object, "selected_right"),
      .mistakes = RequireSize(object, "mistakes", 10'000),
      .revision = RequireSize(object, "revision", 10'000),
      .last_mismatch = RequireBool(object, "last_mismatch"),
  });
}

void ValidatePresentationState(const MiniAppSessionState& session, const QuestionBank& questions) {
  if (session.quiz.answered != session.last_question_id.has_value() ||
      session.quiz.answered != session.last_feedback.has_value()) {
    throw std::invalid_argument("session snapshot feedback state is inconsistent");
  }

  std::optional<std::string> presented_question_id;
  if (session.quiz.answered) {
    presented_question_id = session.last_question_id;
  } else {
    presented_question_id = session.quiz.queue.Current();
  }
  if (!presented_question_id) {
    throw std::invalid_argument("session snapshot has no presented question");
  }
  const auto& question = questions.Get(*presented_question_id);

  if (const auto* single_choice = std::get_if<SingleChoiceTask>(&question.task)) {
    std::vector<std::string> expected;
    for (const auto& option : single_choice->options)
      expected.push_back(option.id);
    auto actual = session.option_order;
    std::ranges::sort(expected);
    std::ranges::sort(actual);
    if (actual != expected || session.matching) {
      throw std::invalid_argument("session snapshot choice presentation is incompatible with the question");
    }
  } else if (const auto* multiple_choice = std::get_if<MultipleChoiceTask>(&question.task)) {
    std::vector<std::string> expected;
    for (const auto& option : multiple_choice->options)
      expected.push_back(option.id);
    auto actual = session.option_order;
    std::ranges::sort(expected);
    std::ranges::sort(actual);
    if (actual != expected || session.matching) {
      throw std::invalid_argument("session snapshot choice presentation is incompatible with the question");
    }
  } else if (std::holds_alternative<ShortAnswerTask>(question.task)) {
    if (!session.option_order.empty() || session.matching) {
      throw std::invalid_argument("session snapshot short-answer presentation is invalid");
    }
  } else if (const auto* matching_task = std::get_if<MatchingTask>(&question.task)) {
    if (!session.option_order.empty() || !session.matching || session.matching->Size() != matching_task->pairs.size()) {
      throw std::invalid_argument("session snapshot matching presentation is incompatible with the question");
    }
  }
}

}  // namespace

std::string SerializeMiniAppSessionSnapshot(const MiniAppSessionState& session) {
  if (session.storage_session_id < 0 || session.snapshot_version < 0 || session.modes.empty()) {
    throw std::invalid_argument("cannot serialize an invalid Mini App session");
  }

  boost::json::array modes;
  modes.reserve(session.modes.size());
  for (const auto mode : session.modes)
    modes.emplace_back(TrainingModeName(mode));

  boost::json::object result{
      {"schema_version", kMiniAppSessionSnapshotSchemaVersion},
      {"quiz", QuizToJson(session.quiz)},
      {"modes", std::move(modes)},
      {"topics", StringsToJson(session.topics)},
      {"selected_level", session.selected_level},
      {"tracks_level_progress", session.tracks_level_progress},
      {"option_order", StringsToJson(session.option_order)},
      {"questions_started", session.questions_started},
      {"current_attempt", session.current_attempt},
      {"current_content_revision", session.current_content_revision},
      {"xp_gained", session.xp_gained},
      {"theory_viewed_question_ids", StringsToJson(session.theory_viewed_question_ids)},
      {"last_question_id",
       session.last_question_id ? boost::json::value(*session.last_question_id) : boost::json::value(nullptr)},
      {"last_feedback",
       session.last_feedback ? boost::json::value(*session.last_feedback) : boost::json::value(nullptr)},
  };
  result["matching"] =
      session.matching ? boost::json::value(MatchingToJson(*session.matching)) : boost::json::value(nullptr);
  return boost::json::serialize(result);
}

MiniAppSessionState ParseMiniAppSessionSnapshot(std::string_view json, std::int64_t storage_session_id,
                                                std::int64_t snapshot_version, const QuestionBank& questions) {
  if (storage_session_id <= 0 || snapshot_version <= 0) {
    throw std::invalid_argument("persisted Mini App session identity is invalid");
  }
  const auto parsed = boost::json::parse(json);
  if (!parsed.is_object()) {
    throw std::invalid_argument("Mini App session snapshot must be a JSON object");
  }
  const auto& object = parsed.as_object();
  if (RequireInt(object, "schema_version", 1, kMiniAppSessionSnapshotSchemaVersion) !=
      kMiniAppSessionSnapshotSchemaVersion) {
    throw std::invalid_argument("unsupported Mini App session snapshot version");
  }

  std::vector<TrainingContentMode> modes;
  for (const auto& value : RequireArray(object, "modes")) {
    if (!value.is_string()) {
      throw std::invalid_argument("session snapshot training mode must be a string");
    }
    const auto mode = ParseTrainingMode(CopyString(value.as_string()));
    if (std::ranges::find(modes, mode) != modes.end()) {
      throw std::invalid_argument("session snapshot contains a duplicate training mode");
    }
    modes.push_back(mode);
  }
  if (modes.empty() || (ContainsTrainingMode(modes, TrainingContentMode::kMatching) && modes.size() != 1)) {
    throw std::invalid_argument("session snapshot contains an invalid training mode combination");
  }

  std::optional<MatchingRound> matching;
  const auto& matching_value = RequireValue(object, "matching");
  if (!matching_value.is_null()) {
    if (!matching_value.is_object()) {
      throw std::invalid_argument("session snapshot matching state must be an object or null");
    }
    matching.emplace(ParseMatching(matching_value.as_object()));
  }

  std::optional<std::string> last_question_id;
  const auto& question_value = RequireValue(object, "last_question_id");
  if (!question_value.is_null()) {
    if (!question_value.is_string()) {
      throw std::invalid_argument("session snapshot last question id must be a string or null");
    }
    last_question_id = CopyString(question_value.as_string());
  }

  std::optional<boost::json::object> last_feedback;
  const auto& feedback_value = RequireValue(object, "last_feedback");
  if (!feedback_value.is_null()) {
    if (!feedback_value.is_object()) {
      throw std::invalid_argument("session snapshot feedback must be an object or null");
    }
    last_feedback = feedback_value.as_object();
  }

  MiniAppSessionState session{
      .quiz = ParseQuiz(RequireObject(object, "quiz"), questions),
      .storage_session_id = storage_session_id,
      .snapshot_version = snapshot_version,
      .modes = std::move(modes),
      .topics = ParseStrings(RequireArray(object, "topics"), "topics"),
      .selected_level = RequireInt(object, "selected_level", 1, 10),
      .tracks_level_progress = RequireBool(object, "tracks_level_progress"),
      .option_order = ParseStrings(RequireArray(object, "option_order"), "option_order"),
      .matching = std::move(matching),
      .questions_started = RequireSize(object, "questions_started", 100),
      .current_attempt = RequireSize(object, "current_attempt", 1'000),
      .xp_gained = RequireInt64(object, "xp_gained"),
      .theory_viewed_question_ids =
          ParseStrings(RequireArray(object, "theory_viewed_question_ids"), "theory_viewed_question_ids"),
      .last_question_id = std::move(last_question_id),
      .last_feedback = std::move(last_feedback),
  };
  if (const auto* revision = object.if_contains("current_content_revision")) {
    if (!revision->is_string() || revision->as_string().size() > 64) {
      throw std::invalid_argument("invalid presented content revision");
    }
    session.current_content_revision = CopyString(revision->as_string());
  }
  if (session.current_attempt == 0 || session.current_attempt > 1'000 || session.xp_gained < 0) {
    throw std::invalid_argument("session snapshot contains invalid attempt or XP counters");
  }
  std::unordered_set<std::string_view> unique_theory_questions;
  for (const auto& question_id : session.theory_viewed_question_ids) {
    static_cast<void>(questions.Get(question_id));
    if (!unique_theory_questions.insert(question_id).second) {
      throw std::invalid_argument("session snapshot contains a duplicate viewed theory question");
    }
  }
  ValidatePresentationState(session, questions);
  return session;
}
