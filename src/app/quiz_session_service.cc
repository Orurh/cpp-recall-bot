#include "quiz_session_service.h"

#include "learning/adaptive_level.h"
#include "learning/learning_queue.h"
#include "quiz/question.h"
#include "quiz/question_bank.h"
#include "quiz/quiz_service.h"
#include "quiz/session.h"

#include <algorithm>
#include <cstddef>
#include <random>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace {

constexpr int kMinLevel = 1;
constexpr int kMaxLevel = 10;
constexpr int kInitialAdaptiveLevel = 3;
constexpr std::size_t kAdaptiveBatchSize = 3;
constexpr std::size_t kSessionQuestionGoal = 10;

void ValidateTargetLevel(int level) {
  if (level < kMinLevel || level > kMaxLevel) {
    throw std::invalid_argument("adaptive target level must be between 1 and 10");
  }
}

int LevelDistance(int lhs, int rhs) {
  return lhs >= rhs ? lhs - rhs : rhs - lhs;
}

int ReviewBucket(const std::unordered_map<std::string, QuestionReviewState>& review_states,
                 std::string_view question_id) {
  const auto it = review_states.find(std::string(question_id));

  if (it == review_states.end()) {
    return 1;
  }

  return it->second.due ? 0 : 2;
}

QuestionReviewState ReviewStateOrDefault(const std::unordered_map<std::string, QuestionReviewState>& review_states,
                                         std::string_view question_id) {
  const auto it = review_states.find(std::string(question_id));
  return it == review_states.end() ? QuestionReviewState{} : it->second;
}

AdaptiveLevelState TopicStateOrFallback(const std::unordered_map<std::string, AdaptiveLevelState>& topic_states,
                                        std::string_view topic, AdaptiveLevelState fallback_state) {
  ValidateAdaptiveLevelState(fallback_state);

  const auto it = topic_states.find(std::string(topic));

  if (it == topic_states.end()) {
    return fallback_state;
  }

  ValidateAdaptiveLevelState(it->second);
  return it->second;
}

AnswerSubmission BuildSubmission(const QuizSession& session, const Question& question, AnswerResult result) {
  if (result == AnswerResult::kUnknownOption) {
    return {
        .session = session,
        .result = result,
        .attempts = 0,
        .completed = false,
    };
  }

  QuizSession next = session;

  const auto submit_result = next.queue.SubmitAnswer(result == AnswerResult::kCorrect);

  next.answered = true;
  ++next.total_attempts;

  if (result == AnswerResult::kCorrect) {
    ++next.correct_attempts;
    ++next.current_streak;
    next.best_streak = std::max(next.best_streak, next.current_streak);
  } else {
    next.current_streak = 0;
  }

  if (submit_result.completed) {
    if (std::ranges::find(next.completed_question_ids, question.id) == next.completed_question_ids.end()) {
      next.completed_question_ids.push_back(question.id);
    }

    if (result == AnswerResult::kCorrect) {
      ++next.mastered_questions;

      if (submit_result.attempts == 1) {
        ++next.first_try_correct;
      }
    }

    const bool correct = result == AnswerResult::kCorrect;
    const auto current_topic_state = TopicStateOrFallback(session.topic_states, question.topic,
                                                          AdaptiveLevelState{.level = session.adaptive_state.level});

    next.topic_states.insert_or_assign(
        question.topic, ApplyAdaptiveEvidence(current_topic_state, question.level, correct, submit_result.attempts));

    next.adaptive_state =
        ApplyAdaptiveEvidence(session.adaptive_state, question.level, correct, submit_result.attempts);
  }

  return {
      .session = std::move(next),
      .result = result,
      .attempts = submit_result.attempts,
      .completed = submit_result.completed,
  };
}

}  // namespace

QuizSessionService::QuizSessionService(const QuestionBank& questions) : questions_(questions) {}

QuizSession QuizSessionService::Start() const {
  return Start(QuestionSelection{}, kInitialAdaptiveLevel);
}

QuizSession QuizSessionService::Start(const QuestionSelection& selection) const {
  return Start(selection, kInitialAdaptiveLevel);
}

QuizSession QuizSessionService::Start(const QuestionSelection& selection, int target_level) const {
  return Start(selection, target_level, {}, {});
}

QuizSession QuizSessionService::Start(const QuestionSelection& selection, int target_level,
                                      const std::unordered_map<std::string, int>& topic_levels,
                                      std::unordered_map<std::string, QuestionReviewState> review_states) const {
  std::unordered_map<std::string, AdaptiveLevelState> topic_states;
  topic_states.reserve(topic_levels.size());

  for (const auto& [topic, level] : topic_levels) {
    topic_states.emplace(topic, AdaptiveLevelState{.level = level});
  }

  return Start(selection, AdaptiveLevelState{.level = target_level}, std::move(topic_states), std::move(review_states));
}

QuizSession QuizSessionService::Start(const QuestionSelection& selection, AdaptiveLevelState adaptive_state,
                                      std::unordered_map<std::string, AdaptiveLevelState> topic_states,
                                      std::unordered_map<std::string, QuestionReviewState> review_states) const {
  ValidateAdaptiveLevelState(adaptive_state);

  for (const auto& [topic, state] : topic_states) {
    if (topic.empty()) {
      throw std::invalid_argument("topic state key must not be empty");
    }

    ValidateAdaptiveLevelState(state);
  }

  const auto available_questions = questions_.get().Select(selection);

  if (available_questions.empty()) {
    throw std::runtime_error("no questions match selection");
  }

  const auto session_goal = std::min(kSessionQuestionGoal, available_questions.size());

  auto question_ids =
      BuildAdaptiveBatch(selection, adaptive_state.level, topic_states, review_states, {}, session_goal);

  return {
      .queue = LearningQueue(question_ids),
      .selection = selection,
      .completed_question_ids = {},
      .topic_states = std::move(topic_states),
      .review_states = std::move(review_states),
      .session_goal = session_goal,
      .total_attempts = 0,
      .correct_attempts = 0,
      .mastered_questions = 0,
      .first_try_correct = 0,
      .current_streak = 0,
      .best_streak = 0,
      .initial_level = adaptive_state.level,
      .adaptive_state = adaptive_state,
      .answered = false,
  };
}

QuizSession QuizSessionService::Refill(const QuizSession& session) const {
  if (!session.queue.Empty()) {
    throw std::logic_error("cannot refill non-empty quiz queue");
  }

  QuizSession next = session;

  if (next.completed_question_ids.size() >= next.session_goal) {
    next.answered = false;
    return next;
  }

  const auto remaining = next.session_goal - next.completed_question_ids.size();

  const auto question_ids = BuildAdaptiveBatch(next.selection, next.adaptive_state.level, next.topic_states,
                                               next.review_states, next.completed_question_ids, remaining);

  next.queue = LearningQueue(question_ids);
  next.answered = false;

  return next;
}

const Question& QuizSessionService::CurrentQuestion(const QuizSession& session) const {
  const auto question_id = session.queue.Current();

  if (!question_id) {
    throw std::logic_error("quiz session has no current question");
  }

  return GetQuestion(*question_id);
}

const Question& QuizSessionService::GetQuestion(std::string_view id) const {
  return questions_.get().Get(id);
}

bool QuizSessionService::HasQuestions(const QuestionSelection& selection) const {
  return !questions_.get().Select(selection).empty();
}

int QuizSessionService::TopicLevel(const QuizSession& session, std::string_view topic) const {
  return TopicState(session, topic).level;
}

AdaptiveLevelState QuizSessionService::TopicState(const QuizSession& session, std::string_view topic) const {
  return TopicStateOrFallback(session.topic_states, topic, AdaptiveLevelState{.level = session.adaptive_state.level});
}

AnswerSubmission QuizSessionService::SubmitAnswer(const QuizSession& session, std::string_view answer) const {
  const auto& question = CurrentQuestion(session);

  return BuildSubmission(session, question, CheckAnswer(question, answer));
}

AnswerSubmission QuizSessionService::SubmitAnswer(const QuizSession& session,
                                                  std::span<const std::string> answers) const {
  const auto& question = CurrentQuestion(session);

  return BuildSubmission(session, question, CheckAnswer(question, answers));
}

std::vector<std::string> QuizSessionService::BuildAdaptiveBatch(
    const QuestionSelection& selection, int target_level,
    const std::unordered_map<std::string, AdaptiveLevelState>& topic_states,
    const std::unordered_map<std::string, QuestionReviewState>& review_states,
    std::span<const std::string> excluded_ids, std::size_t max_questions) const {
  ValidateTargetLevel(target_level);

  auto question_ids = questions_.get().Select(selection);

  std::erase_if(question_ids, [excluded_ids](const std::string& id) {
    return std::ranges::find(excluded_ids, id) != excluded_ids.end();
  });

  static thread_local std::mt19937 generator{std::random_device{}()};

  std::ranges::shuffle(question_ids, generator);

  std::ranges::stable_sort(question_ids, [this, &selection, target_level, &topic_states, &review_states](
                                             const std::string& lhs, const std::string& rhs) {
    const bool lhs_preferred =
        std::ranges::find(selection.preferred_question_ids, lhs) != selection.preferred_question_ids.end();
    const bool rhs_preferred =
        std::ranges::find(selection.preferred_question_ids, rhs) != selection.preferred_question_ids.end();
    if (lhs_preferred != rhs_preferred) {
      return lhs_preferred;
    }

    const auto lhs_bucket = ReviewBucket(review_states, lhs);
    const auto rhs_bucket = ReviewBucket(review_states, rhs);

    if (lhs_bucket != rhs_bucket) {
      return lhs_bucket < rhs_bucket;
    }

    const auto lhs_review = ReviewStateOrDefault(review_states, lhs);
    const auto rhs_review = ReviewStateOrDefault(review_states, rhs);

    if (lhs_bucket == 0) {
      if (lhs_review.stage != rhs_review.stage) {
        return lhs_review.stage < rhs_review.stage;
      }

      if (lhs_review.failed_count != rhs_review.failed_count) {
        return lhs_review.failed_count > rhs_review.failed_count;
      }
    } else if (lhs_bucket == 2 && lhs_review.seconds_until_due != rhs_review.seconds_until_due) {
      return lhs_review.seconds_until_due < rhs_review.seconds_until_due;
    }

    const auto& lhs_question = questions_.get().Get(lhs);
    const auto& rhs_question = questions_.get().Get(rhs);

    const auto lhs_distance = LevelDistance(
        lhs_question.level,
        TopicStateOrFallback(topic_states, lhs_question.topic, AdaptiveLevelState{.level = target_level}).level);

    const auto rhs_distance = LevelDistance(
        rhs_question.level,
        TopicStateOrFallback(topic_states, rhs_question.topic, AdaptiveLevelState{.level = target_level}).level);

    return lhs_distance < rhs_distance;
  });

  const auto batch_size = std::min(kAdaptiveBatchSize, max_questions);

  if (question_ids.size() > batch_size) {
    question_ids.resize(batch_size);
  }

  return question_ids;
}

AnswerSubmission QuizSessionService::SubmitMatchingAnswer(const QuizSession& session, bool without_mistakes) const {
  const auto& question = CurrentQuestion(session);
  if (!std::holds_alternative<MatchingTask>(question.task)) {
    throw std::logic_error("matching result for a non-matching question");
  }
  return BuildSubmission(session, question, without_mistakes ? AnswerResult::kCorrect : AnswerResult::kIncorrect);
}
