#include "quiz_service.h"

#include <algorithm>

AnswerResult CheckAnswer(const Question& question, std::string_view option_id) {
  auto it = std::ranges::find_if(
      question.options,
      [option_id](const auto& option) { return option.id == option_id; });

  if (it == question.options.end()) {
    return AnswerResult::kUnknownOption;
  }

  return it->id == question.correct_option_id
             ? AnswerResult::kCorrect
             : AnswerResult::kIncorrect;
}