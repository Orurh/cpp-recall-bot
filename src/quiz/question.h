#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
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

enum class QuestionEvidenceType : std::uint8_t {
  kRecognition,
  kApplication,
};

constexpr std::string_view QuestionEvidenceTypeName(QuestionEvidenceType type) {
  switch (type) {
    case QuestionEvidenceType::kRecognition:
      return "recognition";
    case QuestionEvidenceType::kApplication:
      return "application";
  }

  return "unknown";
}

struct Question {
  std::string id;
  std::string topic;
  std::string skill_id;
  std::string question_family_id;
  QuestionEvidenceType evidence_type = QuestionEvidenceType::kRecognition;
  bool transfer = false;
  QuestionKind kind = QuestionKind::kTheory;
  int level = 1;
  std::string standard;
  std::vector<std::string> tags;
  std::string text;
  std::optional<std::string> code;
  Task task;
  std::string explanation;
  std::optional<std::string> source;
  std::string public_id = {};
  std::string content_revision = {};
  std::string content_snapshot_json = {};
};
