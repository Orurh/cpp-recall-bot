#pragma once

#include "app/training_mode.h"
#include "learning/progress_summary.h"
#include "quiz/matching_round.h"
#include "quiz/question.h"
#include "quiz/quiz_service.h"

#include <boost/json/object.hpp>

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

struct TrainingProgressView {
  std::string_view topic;
  int topic_level = 3;
  int adaptive_level = 3;
  int question_level = 1;
  std::size_t completed_questions = 0;
  std::size_t session_goal = 0;
  std::size_t question_number = 0;
  std::size_t correct_attempts = 0;
  std::size_t total_attempts = 0;
  std::size_t current_streak = 0;
  bool repeat = false;
  std::size_t attempt_number = 1;
};

struct AnswerFeedbackView {
  AnswerResult result = AnswerResult::kIncorrect;
  std::size_t attempts = 0;
  bool completed = false;
  bool show_explanation = false;
  int previous_topic_level = 3;
  int previous_adaptive_level = 3;
};

boost::json::object BuildStartMenuMessage(std::int64_t chat_id, std::span<const TrainingContentMode> selected_modes,
                                          std::span<const std::string> selected_topics, std::string_view notice = {});

boost::json::object BuildStartMenuKeyboard(std::span<const TrainingContentMode> selected_modes,
                                           std::span<const std::string> selected_topics);

boost::json::object BuildTopicMenuMessage(std::int64_t chat_id, std::span<const std::string> selected_topics);

boost::json::object BuildTopicMenuKeyboard(std::span<const std::string> selected_topics);

boost::json::object BuildProgressSummaryMessage(std::int64_t chat_id, const UserProgressSummary& summary);

boost::json::object BuildQuestionMessage(std::int64_t chat_id, const Question& question,
                                         const std::vector<std::string>& option_order,
                                         const std::vector<std::string>& selected_option_ids,
                                         const TrainingProgressView& progress, const MatchingRound* matching = nullptr);

boost::json::object BuildQuestionKeyboard(const Question& question, const std::vector<std::string>& option_order,
                                          const std::vector<std::string>& selected_option_ids,
                                          const MatchingRound* matching = nullptr);

boost::json::object BuildAnswerKeyboard(bool show_explanation_button);

std::string BuildAnswerDisplayText(const Question& question, const std::vector<std::string>& option_order,
                                   std::string_view answer);

std::string BuildAnswerDisplayText(const Question& question, const std::vector<std::string>& option_order,
                                   std::span<const std::string> answers);

std::string BuildAnswerText(const Question& question, const std::vector<std::string>& option_order,
                            std::string_view answer_display, const AnswerFeedbackView& feedback,
                            const TrainingProgressView& progress);
