#include "quiz_service.h"

#include "quiz/question.h"

#include <algorithm>
#include <span>
#include <string>
#include <string_view>
#include <unordered_set>
#include <variant>

namespace {

std::string_view TrimAsciiWhitespace(std::string_view value) {
  constexpr std::string_view kWhitespace = " \t\n\r\f\v";
  const auto first = value.find_first_not_of(kWhitespace);
  if (first == std::string_view::npos) {
    return {};
  }

  const auto last = value.find_last_not_of(kWhitespace);
  return value.substr(first, last - first + 1);
}

std::string RemoveAsciiWhitespace(std::string_view value) {
  constexpr std::string_view kWhitespace = " \t\n\r\f\v";
  std::string normalized;
  normalized.reserve(value.size());

  for (const char character : value) {
    if (kWhitespace.find(character) == std::string_view::npos) {
      normalized.push_back(character);
    }
  }

  return normalized;
}

AnswerResult CheckTask(const SingleChoiceTask& task, std::string_view answer) {
  const auto it = std::ranges::find(task.options, answer, &AnswerOption::id);

  if (it == task.options.end()) {
    return AnswerResult::kUnknownOption;
  }

  return it->id == task.correct_option_id ? AnswerResult::kCorrect : AnswerResult::kIncorrect;
}

AnswerResult CheckTask(const MultipleChoiceTask&, std::string_view) {
  return AnswerResult::kUnknownOption;
}

AnswerResult CheckTask(const MatchingTask&, std::string_view) {
  // Matching answers are checked incrementally by MatchingRound.
  return AnswerResult::kUnknownOption;
}

AnswerResult CheckTask(const ShortAnswerTask& task, std::string_view answer) {
  answer = TrimAsciiWhitespace(answer);
  const auto it = std::ranges::find(task.accepted_answers, answer);

  return it != task.accepted_answers.end() ? AnswerResult::kCorrect : AnswerResult::kIncorrect;
}

bool HasOption(const MultipleChoiceTask& task, std::string_view id) {
  return std::ranges::find(task.options, id, &AnswerOption::id) != task.options.end();
}

AnswerResult CheckMultipleChoice(const MultipleChoiceTask& task, std::span<const std::string> answers) {
  std::unordered_set<std::string_view> selected;

  for (const auto& answer : answers) {
    if (!HasOption(task, answer)) {
      return AnswerResult::kUnknownOption;
    }

    if (!selected.insert(answer).second) {
      return AnswerResult::kUnknownOption;
    }
  }

  if (selected.size() != task.correct_option_ids.size()) {
    return AnswerResult::kIncorrect;
  }

  for (const auto& correct : task.correct_option_ids) {
    if (!selected.contains(correct)) {
      return AnswerResult::kIncorrect;
    }
  }

  return AnswerResult::kCorrect;
}

}  // namespace

AnswerResult CheckAnswer(const Question& question, std::string_view answer) {
  if (question.kind == QuestionKind::kImplementation) {
    if (const auto* task = std::get_if<ShortAnswerTask>(&question.task); task != nullptr) {
      const std::string normalized_answer = RemoveAsciiWhitespace(answer);
      const auto it = std::ranges::find_if(task->accepted_answers, [&](const std::string& accepted) {
        return RemoveAsciiWhitespace(accepted) == normalized_answer;
      });
      return it != task->accepted_answers.end() ? AnswerResult::kCorrect : AnswerResult::kIncorrect;
    }
  }

  return std::visit([answer](const auto& task) { return CheckTask(task, answer); }, question.task);
}

AnswerResult CheckAnswer(const Question& question, std::span<const std::string> answers) {
  const auto* task = std::get_if<MultipleChoiceTask>(&question.task);

  if (task == nullptr) {
    return AnswerResult::kUnknownOption;
  }

  return CheckMultipleChoice(*task, answers);
}
