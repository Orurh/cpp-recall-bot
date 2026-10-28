#include "learning/learning_route_catalog.h"
#include "learning/reference_catalog.h"
#include "learning/skill_catalog.h"
#include "learning/theory_catalog.h"
#include "quiz/question.h"
#include "quiz/question_bank.h"
#include "quiz/topic_catalog.h"
#include "storage/question_loader.h"

#include <boost/json.hpp>

#include <cstdlib>
#include <exception>
#include <filesystem>
#include <iostream>
#include <map>
#include <string>
#include <string_view>
#include <unordered_set>
#include <utility>
#include <variant>
#include <vector>

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

// Metadata only: no answers, code, explanations or pilot control materials.
// Use the same catalog and revision-aware bindings as the application.
boost::json::object CoverageCatalog(const std::vector<Question>& questions) {
  ValidateLearningRouteCatalog(QuestionBank(questions));
  boost::json::array topics, skills, cards, references, theory, routes;
  for (const auto& topic : TrainingTopics())
    topics.emplace_back(boost::json::object{{"id", topic.id}, {"title", topic.title}});
  for (const auto& skill : SkillCatalog()) {
    boost::json::array prerequisites;
    for (const auto id : skill.prerequisites)
      prerequisites.emplace_back(id);
    skills.emplace_back(boost::json::object{{"id", skill.id},
                                            {"topic", skill.topic},
                                            {"title", skill.title},
                                            {"assessable", IsMasteryPilotSkill(skill.id)},
                                            {"prerequisites", std::move(prerequisites)}});
  }
  for (const auto& question : questions) {
    boost::json::array linked;
    for (const auto* entry : ReferencesForQuestion(question))
      linked.emplace_back(entry->id);
    cards.emplace_back(boost::json::object{{"id", question.id},
                                           {"public_id", question.public_id},
                                           {"content_revision", question.content_revision},
                                           {"topic", question.topic},
                                           {"standard", question.standard},
                                           {"skill_id", question.skill_id},
                                           {"family_id", question.question_family_id},
                                           {"level", question.level},
                                           {"kind", KindName(question.kind)},
                                           {"task", TaskName(question.task)},
                                           {"evidence_type", QuestionEvidenceTypeName(question.evidence_type)},
                                           {"transfer", question.transfer},
                                           {"references", std::move(linked)}});
  }
  for (const auto& entry : ReferenceCatalog())
    references.emplace_back(boost::json::object{
        {"id", entry.id}, {"title", entry.title}, {"content_revision", ReferenceContentRevision(entry)}});
  for (const auto& block : TheoryCatalog())
    theory.emplace_back(boost::json::object{{"id", block.id}, {"skill_id", block.skill_id}, {"title", block.title}});
  for (const auto& route : LearningRouteCatalog()) {
    boost::json::array stages;
    for (const auto& stage : route.stages) {
      boost::json::array ids;
      for (const auto id : stage.question_ids)
        ids.emplace_back(id);
      stages.emplace_back(
          boost::json::object{{"id", stage.id},
                              {"title", stage.title},
                              {"kind", stage.kind == LearningRouteStageKind::kPractice ? "practice" : "independent"},
                              {"question_ids", std::move(ids)}});
    }
    routes.emplace_back(boost::json::object{{"id", route.id},
                                            {"title", route.title},
                                            {"retention_skill_id", route.retention_skill_id},
                                            {"requires_retention", route.requires_retention},
                                            {"stages", std::move(stages)}});
  }
  return {{"schema_version", 1},           {"topics", std::move(topics)},         {"skills", std::move(skills)},
          {"questions", std::move(cards)}, {"references", std::move(references)}, {"theory", std::move(theory)},
          {"routes", std::move(routes)}};
}

}  // namespace

int main(int argc, char* argv[]) {
  const bool coverage = argc > 1 && std::string_view(argv[1]) == "--coverage-json";
  if (argc > (coverage ? 3 : 2)) {
    std::cerr << "usage: content_check "
                 "[--coverage-json] [questions_directory]\n";

    return EXIT_FAILURE;
  }

  const auto position = coverage ? 2 : 1;
  const std::filesystem::path directory = argc > position ? argv[position] : "content/questions";

  try {
    const auto questions = LoadQuestions(directory);
    ValidateTheoryCatalog();
    ValidateReferenceCatalog(questions);
    if (coverage) {
      std::cout << boost::json::serialize(CoverageCatalog(questions)) << '\n';
      return EXIT_SUCCESS;
    }

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
