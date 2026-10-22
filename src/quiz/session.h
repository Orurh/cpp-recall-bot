#pragma once

#include "learning/adaptive_level.h"
#include "learning/learning_queue.h"
#include "learning/review_state.h"
#include "quiz/question_bank.h"

#include <cstddef>
#include <string>
#include <unordered_map>
#include <vector>

struct QuizSession {
  LearningQueue queue;
  QuestionSelection selection;
  std::vector<std::string> completed_question_ids;
  std::unordered_map<std::string, AdaptiveLevelState> topic_states;
  std::unordered_map<std::string, QuestionReviewState> review_states;
  std::size_t session_goal = 0;
  std::size_t total_attempts = 0;
  std::size_t correct_attempts = 0;
  std::size_t mastered_questions = 0;
  std::size_t first_try_correct = 0;
  std::size_t current_streak = 0;
  std::size_t best_streak = 0;
  int initial_level = 3;
  AdaptiveLevelState adaptive_state;
  bool answered = false;
};
