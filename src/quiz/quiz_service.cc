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
  // Line splicing precedes comment recognition in C++. Do not silently accept
  // a token sequence hidden by a continued // comment. These short exercises
  // do not support physical backslash-newline continuations (including C++23
  // whitespace between the backslash and newline); this is not a preprocessor.
  for (std::size_t index = 0; index < value.size(); ++index) {
    if (value[index] != '\\') {
      continue;
    }
    auto next = index + 1;
    while (next < value.size() &&
           (value[next] == ' ' || value[next] == '\t' || value[next] == '\f' || value[next] == '\v')) {
      ++next;
    }
    if (next < value.size() && (value[next] == '\n' || value[next] == '\r')) {
      return std::nullopt;
    }
  }
  constexpr std::array<std::string_view, 1> kFourCharacterPunctuators{"%:%:"};
  constexpr std::array<std::string_view, 5> kThreeCharacterPunctuators{"<=>", ">>=", "<<=", "->*", "..."};
  constexpr std::array<std::string_view, 27> kTwoCharacterPunctuators{
      "::", ".*", "->", "++", "--", "<<", ">>", "<=", ">=", "==", "!=", "&&", "||", "+=",
      "-=", "*=", "/=", "%=", "^=", "&=", "|=", "##", "<:", ":>", "<%", "%>", "%:"};

  std::vector<std::string_view> tokens;
  std::size_t position = 0;
  bool at_logical_line_start = true;
  while (position < value.size()) {
    if (IsAsciiWhitespace(value[position])) {
      if (value[position] == '\n' || value[position] == '\r') {
        at_logical_line_start = true;
      }
      ++position;
      continue;
    }

    if (value.substr(position).starts_with("//")) {
      const auto end = value.find('\n', position + 2);
      position = end == std::string_view::npos ? value.size() : end + 1;
      at_logical_line_start = end != std::string_view::npos;
      continue;
    }
    if (value.substr(position).starts_with("/*")) {
      const auto end = value.find("*/", position + 2);
      if (end == std::string_view::npos) {
        return std::nullopt;
      }
      const auto comment = value.substr(position, end + 2 - position);
      if (comment.find_first_of("\r\n") != std::string_view::npos) {
        at_logical_line_start = true;
      }
      position = end + 2;
      continue;
    }

    // Newlines are semantically significant for preprocessing directives.
    // Short implementation answers deliberately do not support directives;
    // full snippets will later be checked by a compiler in an isolated runner.
    if (at_logical_line_start && (value[position] == '#' || value.substr(position).starts_with("%:"))) {
      return std::nullopt;
    }
    at_logical_line_start = false;

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
    for (const auto punctuator : kFourCharacterPunctuators) {
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

// Only normalize grouping in the small expression grammar used for member
// access exercises: names, unary &/*, member access and calls. Do not erase
// arbitrary parentheses in types, folds or arithmetic, or rewrite operators.
class MemberExpressionParser {
 public:
  explicit MemberExpressionParser(std::span<const std::string_view> tokens) : tokens_(tokens) {}

  std::optional<std::vector<std::string_view>> Parse() {
    std::vector<std::string_view> result;
    const bool statement = Take("return");
    if (statement) {
      result.push_back("return");
    }
    if (!Expression(result, 0) || (statement && !Take(";")) || position_ != tokens_.size()) {
      return std::nullopt;
    }
    return result;
  }

 private:
  bool Take(std::string_view token) {
    if (position_ == tokens_.size() || tokens_[position_] != token) {
      return false;
    }
    ++position_;
    return true;
  }

  bool Name(std::vector<std::string_view>& result) {
    if (position_ == tokens_.size() || tokens_[position_].empty() || !IsIdentifierStart(tokens_[position_].front())) {
      return false;
    }
    result.push_back(tokens_[position_++]);
    return true;
  }

  bool Expression(std::vector<std::string_view>& result, unsigned depth) {
    if (depth >= 64 || position_ == tokens_.size()) {
      return false;
    }
    if (tokens_[position_] == "&" || tokens_[position_] == "*") {
      const auto operation = tokens_[position_++];
      if (!Expression(result, depth + 1)) {
        return false;
      }
      result.push_back(operation);
      return true;
    }
    if (Take("(")) {
      if (!Expression(result, depth + 1) || !Take(")")) {
        return false;
      }
    } else if (!Name(result)) {
      return false;
    }
    while (position_ < tokens_.size()) {
      if (tokens_[position_] == "." || tokens_[position_] == "->") {
        result.push_back(tokens_[position_++]);
        if (!Name(result)) {
          return false;
        }
      } else if (Take("(")) {
        result.push_back("(");
        if (!Take(")")) {
          do {
            if (!Expression(result, depth + 1)) {
              return false;
            }
            if (position_ < tokens_.size() && tokens_[position_] == ",") {
              result.push_back(",");
            }
          } while (Take(","));
          if (!Take(")")) {
            return false;
          }
        }
        result.push_back(")");
      } else {
        break;
      }
    }
    return true;
  }

  std::span<const std::string_view> tokens_;
  std::size_t position_ = 0;
};

bool FullyParenthesized(std::span<const std::string_view> tokens) {
  if (tokens.size() < 2 || tokens.front() != "(" || tokens.back() != ")") {
    return false;
  }
  unsigned depth = 0;
  for (std::size_t index = 0; index < tokens.size(); ++index) {
    if (tokens[index] == "(") {
      ++depth;
    } else if (tokens[index] == ")") {
      if (depth == 0 || (--depth == 0 && index + 1 != tokens.size())) {
        return false;
      }
    }
  }
  return depth == 0;
}

std::optional<std::vector<std::string_view>> CanonicalFold(std::span<const std::string_view> tokens) {
  std::vector<std::string_view> result;
  const bool statement = !tokens.empty() && tokens.front() == "return";
  if (statement) {
    result.push_back("return");
    tokens = tokens.subspan(1);
  }
  const bool semicolon = !tokens.empty() && tokens.back() == ";";
  if (semicolon) {
    tokens = tokens.first(tokens.size() - 1);
  } else if (statement) {
    return std::nullopt;
  }
  if (!FullyParenthesized(tokens)) {
    return std::nullopt;
  }
  tokens = tokens.subspan(1, tokens.size() - 2);
  unsigned grouping_depth = 1;
  while (FullyParenthesized(tokens)) {
    if (++grouping_depth >= 64) {
      return std::nullopt;
    }
    tokens = tokens.subspan(1, tokens.size() - 2);
  }
  const auto ellipsis = std::ranges::find(tokens, "...");
  if (ellipsis == tokens.end() || std::ranges::count(tokens, "...") != 1) {
    return std::nullopt;
  }
  const auto index = static_cast<std::size_t>(ellipsis - tokens.begin());
  const auto left = tokens.first(index);
  const auto right = tokens.subspan(index + 1);
  const auto operation = left.empty() ? (right.empty() ? std::string_view{} : right.front()) : left.back();
  if ((operation != "+" && operation != "&&") || (!right.empty() && right.front() != operation)) {
    return std::nullopt;
  }
  result.push_back("fold(");
  if (!left.empty()) {
    const auto operand = MemberExpressionParser(left.first(left.size() - 1)).Parse();
    if (!operand) {
      return std::nullopt;
    }
    result.insert(result.end(), operand->begin(), operand->end());
    result.push_back(operation);
  }
  result.push_back("...");
  if (!right.empty()) {
    const auto operand = MemberExpressionParser(right.subspan(1)).Parse();
    if (!operand) {
      return std::nullopt;
    }
    result.push_back(operation);
    result.insert(result.end(), operand->begin(), operand->end());
  }
  result.push_back(")");
  if (semicolon) {
    result.push_back(";");
  }
  return result;
}

bool EquivalentImplementationTokens(std::span<const std::string_view> expected,
                                    std::span<const std::string_view> answer) {
  if (std::ranges::equal(expected, answer)) {
    return true;
  }
  if (std::ranges::find(expected, "...") != expected.end()) {
    const auto expected_fold = CanonicalFold(expected);
    const auto answer_fold = CanonicalFold(answer);
    return expected_fold && answer_fold && *expected_fold == *answer_fold;
  }
  const bool member_expression =
      std::ranges::find(expected, ".") != expected.end() || std::ranges::find(expected, "->") != expected.end();
  if (!member_expression) {
    return false;
  }
  const auto expected_expression = MemberExpressionParser(expected).Parse();
  const auto answer_expression = MemberExpressionParser(answer).Parse();
  return expected_expression && answer_expression && *expected_expression == *answer_expression;
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
        return accepted_tokens && EquivalentImplementationTokens(*accepted_tokens, *answer_tokens);
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
