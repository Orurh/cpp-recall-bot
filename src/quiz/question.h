#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <variant>
#include <vector>

struct AnswerOption {
  std::string id;
  std::string text;
};

struct SingleChoiceTask {
  std::vector<AnswerOption> options;
  std::string correct_option_id;
};

struct MultipleChoiceTask {
  std::vector<AnswerOption> options;
  std::vector<std::string> correct_option_ids;
};

struct ShortAnswerTask {
  std::vector<std::string> accepted_answers;
};

struct MatchingPair {
  std::string id;
  std::string left;
  std::string right;
};

struct MatchingTask {
  std::vector<MatchingPair> pairs;
};

using Task = std::variant<SingleChoiceTask, MultipleChoiceTask, ShortAnswerTask, MatchingTask>;

enum class QuestionKind : std::uint8_t {
  kTheory,
  kApiKnowledge,
  kCodeReading,
  kDebugging,
  kImplementation,
};

struct Question {
  std::string id;
  std::string topic;
  QuestionKind kind = QuestionKind::kTheory;
  int level = 1;
  std::string standard;
  std::vector<std::string> tags;
  std::string text;
  std::optional<std::string> code;
  Task task;
  std::string explanation;
  std::optional<std::string> source;
};
