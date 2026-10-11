#pragma once

#include "learning/review_state.h"
#include "quiz/question.h"
#include "quiz/question_bank.h"
#include "quiz/quiz_service.h"
#include "quiz/session.h"

#include <cstddef>
#include <functional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>

struct AnswerSubmission {
  QuizSession session;
  AnswerResult result = AnswerResult::kIncorrect;
  std::size_t attempts = 0;
  bool completed = false;
};

class QuizSessionService {
 public:
  explicit QuizSessionService(const QuestionBank& questions);

  [[nodiscard]]
  QuizSession Start() const;

  [[nodiscard]]
  QuizSession Start(const QuestionSelection& selection) const;

  [[nodiscard]]
  QuizSession Start(const QuestionSelection& selection, int target_level) const;

  [[nodiscard]]
  QuizSession Start(const QuestionSelection& selection, int target_level,
                    std::unordered_map<std::string, int> topic_levels,
                    std::unordered_map<std::string, QuestionReviewState> review_states = {}) const;

  [[nodiscard]]
  QuizSession Refill(const QuizSession& session) const;

  [[nodiscard]]
  const Question& CurrentQuestion(const QuizSession& session) const;

  [[nodiscard]]
  const Question& GetQuestion(std::string_view id) const;

  [[nodiscard]]
  int TopicLevel(const QuizSession& session, std::string_view topic) const;

  [[nodiscard]]
  AnswerSubmission SubmitAnswer(const QuizSession& session, std::string_view answer) const;

  [[nodiscard]]
  AnswerSubmission SubmitAnswer(const QuizSession& session, std::span<const std::string> answers) const;

  [[nodiscard]]
  AnswerSubmission SubmitMatchingAnswer(
      const QuizSession& session,
      bool without_mistakes) const;

 private:
  [[nodiscard]]
  std::vector<std::string> BuildAdaptiveBatch(const QuestionSelection& selection, int target_level,
                                              const std::unordered_map<std::string, int>& topic_levels,
                                              const std::unordered_map<std::string, QuestionReviewState>& review_states,
                                              std::span<const std::string> excluded_ids,
                                              std::size_t max_questions) const;

  std::reference_wrapper<const QuestionBank> questions_;
};
