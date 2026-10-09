#pragma once

#include "quiz/question.h"

#include <cstdint>
#include <span>
#include <string>
#include <string_view>

enum class AnswerResult : std::int8_t {
  kCorrect,
  kIncorrect,
  kUnknownOption,
};

AnswerResult CheckAnswer(
    const Question& question,
    std::string_view answer);

AnswerResult CheckAnswer(
    const Question& question,
    std::span<const std::string> answers);
