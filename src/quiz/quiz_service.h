#pragma once

#include "quiz/question.h"

#include <string_view>

enum class AnswerResult : int8_t{
  kCorrect,
  kIncorrect,
  kUnknownOption,
};

AnswerResult CheckAnswer(const Question& question, std::string_view option_id);