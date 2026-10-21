#include "quiz/question.h"
#include "storage/question_loader.h"

#include <cstdlib>
#include <exception>
#include <filesystem>
#include <iostream>
#include <map>
#include <string>
#include <string_view>
#include <variant>

namespace {

std::string_view KindName(
    QuestionKind kind) {
  switch (kind) {
    case QuestionKind::kTheory:
      return "theory";

    case QuestionKind::kApiKnowledge:
      return "api_knowledge";

    case QuestionKind::kCodeReading:
      return "code_reading";

    case QuestionKind::kDebugging:
      return "debugging";

    case QuestionKind::kImplementation:
      return "implementation";
  }

  return "unknown";
}

std::string_view TaskName(
    const Task& task) {
  if (std::holds_alternative<
          SingleChoiceTask>(task)) {
    return "single_choice";
  }

  if (std::holds_alternative<
          MultipleChoiceTask>(task)) {
    return "multiple_choice";
  }

  if (std::holds_alternative<MatchingTask>(task)) {
    return "matching";
  }

  return "short_answer";
}

}  // namespace

int main(int argc, char* argv[]) {
  if (argc > 2) {
    std::cerr
        << "usage: content_check "
           "[questions_directory]\n";

    return EXIT_FAILURE;
  }

  const std::filesystem::path directory =
      argc == 2
          ? argv[1]
          : "content/questions";

  try {
    const auto questions =
        LoadQuestions(directory);

    std::map<std::string, std::size_t>
        topics;

    std::map<int, std::size_t>
        levels;

    std::map<std::string_view, std::size_t>
        kinds;

    std::map<std::string_view, std::size_t>
        tasks;

    for (const auto& question :
         questions) {
      ++topics[question.topic];
      ++levels[question.level];
      ++kinds[
          KindName(question.kind)];
      ++tasks[
          TaskName(question.task)];
    }

    std::cout
        << "content-check: OK\n\n";

    std::cout
        << "questions: "
        << questions.size()
        << "\n\n";

    std::cout << "topics:\n";

    for (const auto& [topic, count] :
         topics) {
      std::cout
          << "  "
          << topic
          << ": "
          << count
          << '\n';
    }

    std::cout << "\nlevels:\n";

    for (const auto& [level, count] :
         levels) {
      std::cout
          << "  "
          << level
          << ": "
          << count
          << '\n';
    }

    std::cout << "\nkinds:\n";

    for (const auto& [kind, count] :
         kinds) {
      std::cout
          << "  "
          << kind
          << ": "
          << count
          << '\n';
    }

    std::cout << "\ntasks:\n";

    for (const auto& [task, count] :
         tasks) {
      std::cout
          << "  "
          << task
          << ": "
          << count
          << '\n';
    }

  } catch (
      const std::exception& error) {
    std::cerr
        << "content-check: FAILED\n"
        << error.what()
        << '\n';

    return EXIT_FAILURE;
  }

  return EXIT_SUCCESS;
}
