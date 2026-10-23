#include "quiz_service.h"

#include "quiz/question.h"

#include <algorithm>
#include <array>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_set>
#include <variant>
#include <vector>

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

bool IsAsciiWhitespace(char value) {
  constexpr std::string_view kWhitespace = " \t\n\r\f\v";
  return kWhitespace.find(value) != std::string_view::npos;
}

bool IsIdentifierStart(char value) {
  return (value >= 'a' && value <= 'z') || (value >= 'A' && value <= 'Z') || value == '_';
}

bool IsIdentifierContinuation(char value) {
  return IsIdentifierStart(value) || (value >= '0' && value <= '9');
}

std::optional<std::size_t> QuotedTokenEnd(std::string_view value, std::size_t quote_position) {
  const char quote = value[quote_position];
  for (std::size_t position = quote_position + 1; position < value.size(); ++position) {
    if (value[position] == '\\') {
      ++position;
      continue;
    }
    if (value[position] == quote) {
      return position + 1;
    }
  }
  return std::nullopt;
}

std::optional<std::size_t> RawStringTokenEnd(std::string_view value, std::size_t quote_position) {
  const auto opening_parenthesis = value.find('(', quote_position + 1);
  if (opening_parenthesis == std::string_view::npos || opening_parenthesis - quote_position - 1 > 16) {
    return std::nullopt;
  }

  const auto delimiter = value.substr(quote_position + 1, opening_parenthesis - quote_position - 1);
  std::string terminator = ")";
  terminator.append(delimiter);
  terminator.push_back('"');
  const auto closing = value.find(terminator, opening_parenthesis + 1);
  return closing == std::string_view::npos ? std::nullopt : std::optional<std::size_t>(closing + terminator.size());
}

std::optional<std::size_t> LiteralTokenEnd(std::string_view value, std::size_t position) {
  constexpr std::array<std::string_view, 5> kRawPrefixes{"u8R\"", "uR\"", "UR\"", "LR\"", "R\""};
  for (const auto prefix : kRawPrefixes) {
    if (value.substr(position).starts_with(prefix)) {
      return RawStringTokenEnd(value, position + prefix.size() - 1);
    }
  }

  constexpr std::array<std::string_view, 10> kQuotedPrefixes{"u8\"", "u8'", "u\"", "u'", "U\"",
                                                             "U'",   "L\"", "L'",  "\"", "'"};
  for (const auto prefix : kQuotedPrefixes) {
    if (value.substr(position).starts_with(prefix)) {
      return QuotedTokenEnd(value, position + prefix.size() - 1);
    }
  }
  return std::nullopt;
}

std::optional<std::vector<std::string_view>> TokenizeCpp(std::string_view value) {
  constexpr std::array<std::string_view, 4> kThreeCharacterPunctuators{"<=>", ">>=", "<<=", "->*"};
  constexpr std::array<std::string_view, 19> kTwoCharacterPunctuators{
      "::", "->", "++", "--", "<<", ">>", "<=", ">=", "==", "!=", "&&", "||", "+=", "-=", "*=", "/=", "%=", "&=", "|="};

  std::vector<std::string_view> tokens;
  std::size_t position = 0;
  while (position < value.size()) {
    if (IsAsciiWhitespace(value[position])) {
      ++position;
      continue;
    }

    if (value.substr(position).starts_with("//")) {
      const auto end = value.find('\n', position + 2);
      position = end == std::string_view::npos ? value.size() : end + 1;
      continue;
    }
    if (value.substr(position).starts_with("/*")) {
      const auto end = value.find("*/", position + 2);
      if (end == std::string_view::npos) {
        return std::nullopt;
      }
      position = end + 2;
      continue;
    }

    if (const auto literal_end = LiteralTokenEnd(value, position)) {
      tokens.push_back(value.substr(position, *literal_end - position));
      position = *literal_end;
      continue;
    }

    if (IsIdentifierStart(value[position])) {
      const auto start = position++;
      while (position < value.size() && IsIdentifierContinuation(value[position])) {
        ++position;
      }
      tokens.push_back(value.substr(start, position - start));
      continue;
    }

    if (value[position] >= '0' && value[position] <= '9') {
      const auto start = position++;
      while (position < value.size()) {
        const char current = value[position];
        const bool exponent_sign = (current == '+' || current == '-') && position > start &&
                                   (value[position - 1] == 'e' || value[position - 1] == 'E' ||
                                    value[position - 1] == 'p' || value[position - 1] == 'P');
        if (!IsIdentifierContinuation(current) && current != '.' && current != '\'' && !exponent_sign) {
          break;
        }
        ++position;
      }
      tokens.push_back(value.substr(start, position - start));
      continue;
    }

    bool punctuator_found = false;
    for (const auto punctuator : kThreeCharacterPunctuators) {
      if (value.substr(position).starts_with(punctuator)) {
        tokens.push_back(value.substr(position, punctuator.size()));
        position += punctuator.size();
        punctuator_found = true;
        break;
      }
    }
    if (punctuator_found) {
      continue;
    }
    if (value.substr(position).starts_with("...")) {
      tokens.push_back(value.substr(position, 3));
      position += 3;
      continue;
    }
    for (const auto punctuator : kTwoCharacterPunctuators) {
      if (value.substr(position).starts_with(punctuator)) {
        tokens.push_back(value.substr(position, punctuator.size()));
        position += punctuator.size();
        punctuator_found = true;
        break;
      }
    }
    if (punctuator_found) {
      continue;
    }

    tokens.push_back(value.substr(position, 1));
    ++position;
  }
  return tokens;
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
      const auto answer_tokens = TokenizeCpp(answer);
      if (!answer_tokens) {
        return AnswerResult::kIncorrect;
      }
      const auto it = std::ranges::find_if(task->accepted_answers, [&](const std::string& accepted) {
        const auto accepted_tokens = TokenizeCpp(accepted);
        return accepted_tokens && *accepted_tokens == *answer_tokens;
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
