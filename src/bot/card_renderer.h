#pragma once

#include "app/training_mode.h"
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
  std::size_t completed_questions = 0;
  std::size_t session_goal = 0;
  std::size_t question_number = 0;
  std::size_t correct_attempts = 0;
  std::size_t total_attempts = 0;
  std::size_t current_streak = 0;
  bool repeat = false;
};

boost::json::object BuildStartMenuMessage(
    std::int64_t chat_id,
    std::span<const TrainingContentMode> selected_modes,
    std::string_view notice = {});

boost::json::object BuildStartMenuKeyboard(
    std::span<const TrainingContentMode> selected_modes);

boost::json::object BuildQuestionMessage(
    std::int64_t chat_id,
    const Question& question,
    const std::vector<std::string>& option_order,
    const std::vector<std::string>& selected_option_ids,
    const TrainingProgressView& progress);

boost::json::object BuildQuestionKeyboard(
    const Question& question,
    const std::vector<std::string>& option_order,
    const std::vector<std::string>& selected_option_ids);

boost::json::object BuildAnswerKeyboard(
    bool show_explanation_button);

std::string BuildAnswerDisplayText(
    const Question& question,
    const std::vector<std::string>& option_order,
    std::string_view answer);

std::string BuildAnswerDisplayText(
    const Question& question,
    const std::vector<std::string>& option_order,
    std::span<const std::string> answers);

std::string BuildAnswerText(
    const Question& question,
    const std::vector<std::string>& option_order,
    std::string_view answer_display,
    AnswerResult result,
    bool show_explanation,
    const TrainingProgressView& progress);
