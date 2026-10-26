#include "learning/reference_catalog.h"
#include "learning/skill_catalog.h"
#include "learning/theory_catalog.h"
#include "quiz/question.h"
#include "storage/question_loader.h"

#include <cstdlib>
#include <exception>
#include <filesystem>
#include <iostream>
#include <map>
#include <string>
#include <string_view>
#include <unordered_set>
#include <variant>

namespace {

std::string_view KindName(QuestionKind kind) {
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

std::string_view TaskName(const Task& task) {
  if (std::holds_alternative<SingleChoiceTask>(task)) {
    return "single_choice";
  }

  if (std::holds_alternative<MultipleChoiceTask>(task)) {
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
    std::cerr << "usage: content_check "
                 "[questions_directory]\n";

    return EXIT_FAILURE;
  }

  const std::filesystem::path directory = argc == 2 ? argv[1] : "content/questions";

  try {
    const auto questions = LoadQuestions(directory);
    ValidateTheoryCatalog();
    ValidateReferenceCatalog(questions);

    std::map<std::string, std::size_t> topics;

    std::map<int, std::size_t> levels;

    std::map<std::string_view, std::size_t> kinds;

    std::map<std::string_view, std::size_t> tasks;

    std::map<std::string_view, std::size_t> skills;

    std::map<std::string_view, std::size_t> evidence_types;

    std::unordered_set<std::string_view> families;

    std::size_t transfer_questions = 0;

    for (const auto& skill : SkillCatalog()) {
      skills.emplace(skill.id, 0);
    }

    for (const auto& question : questions) {
      ++topics[question.topic];
      ++levels[question.level];
      ++kinds[KindName(question.kind)];
      ++tasks[TaskName(question.task)];
      ++skills[question.skill_id];
      ++evidence_types[QuestionEvidenceTypeName(question.evidence_type)];
      families.insert(question.question_family_id);
      if (question.transfer) {
        ++transfer_questions;
      }
    }

    std::cout << "content-check: OK\n\n";

    std::cout << "questions: " << questions.size() << "\n\n";
    std::cout << "theory blocks: " << TheoryCatalog().size() << "\n\n";

    std::cout << "topics:\n";

    for (const auto& [topic, count] : topics) {
      std::cout << "  " << topic << ": " << count << '\n';
    }

    std::cout << "\nlevels:\n";

    for (const auto& [level, count] : levels) {
      std::cout << "  " << level << ": " << count << '\n';
    }

    std::cout << "\nkinds:\n";

    for (const auto& [kind, count] : kinds) {
      std::cout << "  " << kind << ": " << count << '\n';
    }

    std::cout << "\ntasks:\n";

    for (const auto& [task, count] : tasks) {
      std::cout << "  " << task << ": " << count << '\n';
    }

    std::cout << "\nevidence:\n";

    for (const auto& [evidence_type, count] : evidence_types) {
      std::cout << "  " << evidence_type << ": " << count << '\n';
    }

    std::cout << "  transfer: " << transfer_questions << '\n';
    std::cout << "  families: " << families.size() << '\n';

    std::cout << "\nskills:\n";

    for (const auto& [skill, count] : skills) {
      std::cout << "  " << skill << ": " << count << '\n';
    }

  } catch (const std::exception& error) {
    std::cerr << "content-check: FAILED\n" << error.what() << '\n';

    return EXIT_FAILURE;
  }

  return EXIT_SUCCESS;
}
