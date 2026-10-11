#include "question_loader.h"

#include "quiz/question.h"
#include "quiz/question_validator.h"

#include <boost/json.hpp>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {

std::string ReadRequiredString(const boost::json::object& object, const char* key) {
  const auto& value = object.at(key).as_string();

  return {
      value.data(),
      value.size(),
  };
}

std::optional<std::string> ReadOptionalString(const boost::json::object& object, const char* key) {
  const auto* value = object.if_contains(key);

  if (value == nullptr || value->is_null()) {
    return std::nullopt;
  }

  const auto& text = value->as_string();

  return std::string(text.data(), text.size());
}

std::vector<std::string> ReadStringArray(const boost::json::object& object, const char* key) {
  const auto& array = object.at(key).as_array();

  std::vector<std::string> values;
  values.reserve(array.size());

  for (const auto& item : array) {
    const auto& value = item.as_string();

    values.emplace_back(value.data(), value.size());
  }

  return values;
}

std::vector<AnswerOption> ReadOptions(const boost::json::object& task_object) {
  const auto& array = task_object.at("options").as_array();

  std::vector<AnswerOption> options;
  options.reserve(array.size());

  for (const auto& item : array) {
    const auto& option = item.as_object();

    options.push_back({
        .id = ReadRequiredString(option, "id"),
        .text = ReadRequiredString(option, "text"),
    });
  }

  return options;
}

int ReadLevel(const boost::json::object& object) {
  const auto level = object.at("level").as_int64();

  constexpr int kMinLevel = 1;
  constexpr int kMaxLevel = 10;

  if (level < kMinLevel || level > kMaxLevel) {
    throw std::out_of_range(
        "question level must be "
        "between 1 and 10");
  }

  return static_cast<int>(level);
}

QuestionKind ReadQuestionKind(const boost::json::object& object) {
  const auto& kind = object.at("kind").as_string();

  if (kind == "theory") {
    return QuestionKind::kTheory;
  }

  if (kind == "api_knowledge") {
    return QuestionKind::kApiKnowledge;
  }

  if (kind == "code_reading") {
    return QuestionKind::kCodeReading;
  }

  if (kind == "debugging") {
    return QuestionKind::kDebugging;
  }

  if (kind == "implementation") {
    return QuestionKind::kImplementation;
  }

  throw std::invalid_argument("unknown question kind: " + std::string(kind.data(), kind.size()));
}

std::vector<std::string> ReadTags(const boost::json::object& object) {
  const auto& array = object.at("tags").as_array();

  std::vector<std::string> tags;
  tags.reserve(array.size());

  for (const auto& item : array) {
    const auto& tag = item.as_string();

    tags.emplace_back(tag.data(), tag.size());
  }

  return tags;
}

Task ReadTask(const boost::json::object& object) {
  const auto& task_object = object.at("task").as_object();

  const auto& type = task_object.at("type").as_string();

  if (type == "single_choice") {
    return SingleChoiceTask{
        .options = ReadOptions(task_object),
        .correct_option_id = ReadRequiredString(task_object, "correct_option_id"),
    };
  }

  if (type == "multiple_choice") {
    return MultipleChoiceTask{
        .options = ReadOptions(task_object),
        .correct_option_ids = ReadStringArray(task_object, "correct_option_ids"),
    };
  }

  if (type == "matching") {
    const auto& array = task_object.at("pairs").as_array();
    std::vector<MatchingPair> pairs;
    pairs.reserve(array.size());

    for (const auto& item : array) {
      const auto& pair = item.as_object();
      pairs.push_back({
          .id = ReadRequiredString(pair, "id"),
          .left = ReadRequiredString(pair, "left"),
          .right = ReadRequiredString(pair, "right"),
      });
    }
    return MatchingTask{.pairs = std::move(pairs)};
  }

  if (type == "short_answer") {
    return ShortAnswerTask{
        .accepted_answers = ReadStringArray(task_object, "accepted_answers"),
    };
  }

  throw std::runtime_error("unknown task type: " + std::string(type));
}

}  // namespace

Question LoadQuestion(const std::filesystem::path& path) {
  std::ifstream input(path);

  if (!input) {
    throw std::runtime_error("file does not exist: " + path.string());
  }

  try {
    const auto json = boost::json::parse(input);

    const auto& object = json.as_object();

    Question question{
        .id = ReadRequiredString(object, "id"),
        .topic = ReadRequiredString(object, "topic"),
        .kind = ReadQuestionKind(object),
        .level = ReadLevel(object),
        .standard = ReadRequiredString(object, "standard"),
        .tags = ReadTags(object),
        .text = ReadRequiredString(object, "text"),
        .code = ReadOptionalString(object, "code"),
        .task = ReadTask(object),
        .explanation = ReadRequiredString(object, "explanation"),
        .source = ReadOptionalString(object, "source"),
    };

    ValidateQuestion(question);

    return question;

  } catch (const std::exception& error) {
    throw std::runtime_error(path.string() + ": " + error.what());
  }
}

std::vector<Question> LoadQuestions(const std::filesystem::path& directory) {
  std::vector<std::filesystem::path> paths;

  for (const auto& entry : std::filesystem::recursive_directory_iterator(directory)) {
    if (!entry.is_regular_file() || entry.path().extension() != ".json") {
      continue;
    }

    paths.push_back(entry.path());
  }

  std::ranges::sort(paths);

  std::vector<Question> questions;
  questions.reserve(paths.size());

  for (const auto& path : paths) {
    questions.push_back(LoadQuestion(path));
  }

  ValidateQuestions(questions);

  return questions;
}
