#include "question_validator.h"

#include "learning/module_catalog.h"
#include "learning/skill_catalog.h"
#include "quiz/topic_catalog.h"

#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>
#include <variant>
#include <vector>

namespace {

[[noreturn]]
void ValidationError(const Question& question, std::string_view message) {
  const std::string id = question.id.empty() ? "<empty-id>" : question.id;

  throw std::invalid_argument("question '" + id + "': " + std::string(message));
}

void RequireNotEmpty(const Question& question, std::string_view value, std::string_view field) {
  if (!value.empty()) {
    return;
  }

  ValidationError(question, std::string(field) + " must not be empty");
}

void ValidateTags(const Question& question) {
  if (question.tags.empty()) {
    ValidationError(question, "tags must contain at least one value");
  }

  std::unordered_set<std::string_view> seen;

  for (const auto& tag : question.tags) {
    if (tag.empty()) {
      ValidationError(question, "tag must not be empty");
    }

    if (!seen.insert(tag).second) {
      ValidationError(question, "duplicate tag: " + tag);
    }
  }
}

std::unordered_set<std::string_view> ValidateOptions(const Question& question,
                                                     const std::vector<AnswerOption>& options) {
  constexpr std::size_t k_max_options = 10;

  if (options.size() < 2) {
    ValidationError(question,
                    "choice task must contain "
                    "at least two options");
  }

  if (options.size() > k_max_options) {
    ValidationError(question,
                    "choice task must contain "
                    "at most 10 options");
  }

  std::unordered_set<std::string_view> option_ids;

  for (const auto& option : options) {
    if (option.id.empty()) {
      ValidationError(question, "option id must not be empty");
    }

    if (option.text.empty()) {
      ValidationError(question, "option text must not be empty");
    }

    if (!option_ids.insert(option.id).second) {
      ValidationError(question, "duplicate option id: " + option.id);
    }
  }

  return option_ids;
}

void ValidateSingleChoice(const Question& question, const SingleChoiceTask& task) {
  const auto option_ids = ValidateOptions(question, task.options);

  RequireNotEmpty(question, task.correct_option_id, "correct_option_id");

  if (!option_ids.contains(task.correct_option_id)) {
    ValidationError(question,
                    "correct_option_id does not "
                    "reference an existing option: " +
                        task.correct_option_id);
  }
}

void ValidateMultipleChoice(const Question& question, const MultipleChoiceTask& task) {
  const auto option_ids = ValidateOptions(question, task.options);

  if (task.correct_option_ids.empty()) {
    ValidationError(question,
                    "multiple_choice must contain "
                    "at least one correct option");
  }

  std::unordered_set<std::string_view> correct_ids;

  for (const auto& id : task.correct_option_ids) {
    if (id.empty()) {
      ValidationError(question,
                      "correct option id must not "
                      "be empty");
    }

    if (!correct_ids.insert(id).second) {
      ValidationError(question, "duplicate correct option id: " + id);
    }

    if (!option_ids.contains(id)) {
      ValidationError(question,
                      "correct option id does not "
                      "reference an existing option: " +
                          id);
    }
  }
}

void ValidateMatching(const Question& question, const MatchingTask& task) {
  if (task.pairs.size() < 2 || task.pairs.size() > 6) {
    ValidationError(question, "matching requires between 2 and 6 pairs");
  }

  std::unordered_set<std::string_view> ids;
  std::unordered_set<std::string_view> left_values;
  std::unordered_set<std::string_view> right_values;

  for (const auto& pair : task.pairs) {
    RequireNotEmpty(question, pair.id, "matching pair id");
    RequireNotEmpty(question, pair.left, "matching pair left");
    RequireNotEmpty(question, pair.right, "matching pair right");
    if (!ids.insert(pair.id).second || !left_values.insert(pair.left).second ||
        !right_values.insert(pair.right).second) {
      ValidationError(question, "matching pairs must have unique ids and labels");
    }
  }
}

void ValidateShortAnswer(const Question& question, const ShortAnswerTask& task) {
  if (task.accepted_answers.empty()) {
    ValidationError(question,
                    "short_answer must contain "
                    "at least one accepted answer");
  }

  std::unordered_set<std::string_view> answers;

  for (const auto& answer : task.accepted_answers) {
    if (answer.empty()) {
      ValidationError(question,
                      "accepted answer must not "
                      "be empty");
    }

    if (!answers.insert(answer).second) {
      ValidationError(question, "duplicate accepted answer: " + answer);
    }
  }
}

}  // namespace

void ValidateQuestion(const Question& question) {
  RequireNotEmpty(question, question.id, "id");

  RequireNotEmpty(question, question.topic, "topic");

  if (!IsKnownTopic(question.topic)) {
    ValidationError(question, "unknown topic: " + question.topic);
  }

  RequireNotEmpty(question, question.skill_id, "skill_id");
  const auto* skill = FindSkill(question.skill_id);
  if (skill == nullptr) {
    ValidationError(question, "unknown skill: " + question.skill_id);
  }
  if (skill->topic != question.topic) {
    ValidationError(question, "skill topic does not match question topic: " + question.skill_id);
  }

  RequireNotEmpty(question, question.question_family_id, "question_family_id");
  const std::string family_prefix = question.skill_id + ".";
  if (!question.question_family_id.starts_with(family_prefix) ||
      question.question_family_id.size() == family_prefix.size()) {
    ValidationError(question, "question_family_id must start with skill_id followed by a family name");
  }

  if (question.transfer && question.evidence_type != QuestionEvidenceType::kApplication) {
    ValidationError(question, "transfer evidence must have evidence_type 'application'");
  }

  RequireNotEmpty(question, question.standard, "standard");

  RequireNotEmpty(question, question.text, "text");

  RequireNotEmpty(question, question.explanation, "explanation");

  if ((question.kind == QuestionKind::kCodeReading || question.kind == QuestionKind::kDebugging ||
       question.kind == QuestionKind::kImplementation) &&
      (!question.code || question.code->empty())) {
    ValidationError(question, "applied question kind requires a non-empty code sample");
  }

  if (question.level < 1 || question.level > 10) {
    ValidationError(question, "level must be between 1 and 10");
  }

  ValidateTags(question);

  std::visit(
      [&question](const auto& task) {
        using TaskType = std::decay_t<decltype(task)>;

        if constexpr (std::is_same_v<TaskType, SingleChoiceTask>) {
          ValidateSingleChoice(question, task);

        } else if constexpr (std::is_same_v<TaskType, MultipleChoiceTask>) {
          ValidateMultipleChoice(question, task);

        } else if constexpr (std::is_same_v<TaskType, ShortAnswerTask>) {
          ValidateShortAnswer(question, task);
        } else if constexpr (std::is_same_v<TaskType, MatchingTask>) {
          ValidateMatching(question, task);
        }
      },
      question.task);
}

void ValidateQuestions(const std::vector<Question>& questions) {
  ValidateSkillCatalog();

  if (questions.empty()) {
    throw std::invalid_argument("question bank is empty");
  }

  std::unordered_set<std::string_view> ids;
  std::unordered_map<std::string_view, const Question*> families;

  for (const auto& question : questions) {
    ValidateQuestion(question);

    if (!ids.insert(question.id).second) {
      throw std::invalid_argument("duplicate question id: " + question.id);
    }

    const auto [family, inserted] = families.emplace(question.question_family_id, &question);
    if (!inserted) {
      const auto& first = *family->second;
      if (first.skill_id != question.skill_id || first.evidence_type != question.evidence_type ||
          first.transfer != question.transfer) {
        throw std::invalid_argument("question family has inconsistent evidence metadata: " +
                                    question.question_family_id);
      }
    }
  }

  ValidateLearningModuleCoverage(questions);
}
