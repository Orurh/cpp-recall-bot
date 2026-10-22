#include "question_bank.h"

#include "learning/skill_catalog.h"
#include "quiz/question.h"

#include <algorithm>
#include <cstddef>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace {

void ValidateSelection(const QuestionSelection& selection) {
  constexpr int kMinLevel = 1;
  constexpr int kMaxLevel = 10;

  if (selection.min_level < kMinLevel || selection.max_level > kMaxLevel) {
    throw std::invalid_argument("question selection level must be between 1 and 10");
  }

  if (selection.min_level > selection.max_level) {
    throw std::invalid_argument("question selection min_level must not exceed max_level");
  }

  for (std::size_t i = 0; i < selection.topics.size(); ++i) {
    if (selection.topics[i].empty()) {
      throw std::invalid_argument("question selection topic must not be empty");
    }

    if (std::ranges::find(selection.topics.begin(), selection.topics.begin() + static_cast<std::ptrdiff_t>(i),
                          selection.topics[i]) != selection.topics.begin() + static_cast<std::ptrdiff_t>(i)) {
      throw std::invalid_argument("duplicate question selection topic: " + selection.topics[i]);
    }
  }

  for (std::size_t i = 0; i < selection.skill_ids.size(); ++i) {
    const auto& skill_id = selection.skill_ids[i];
    if (FindSkill(skill_id) == nullptr) {
      throw std::invalid_argument("unknown question selection skill: " + skill_id);
    }
    if (std::ranges::find(selection.skill_ids.begin(), selection.skill_ids.begin() + static_cast<std::ptrdiff_t>(i),
                          skill_id) != selection.skill_ids.begin() + static_cast<std::ptrdiff_t>(i)) {
      throw std::invalid_argument("duplicate question selection skill: " + skill_id);
    }
  }
}

bool Matches(const Question& question, const QuestionSelection& selection) {
  if (question.level < selection.min_level || question.level > selection.max_level) {
    return false;
  }

  if (!selection.topics.empty() && std::ranges::find(selection.topics, question.topic) == selection.topics.end()) {
    return false;
  }

  if (!selection.skill_ids.empty() &&
      std::ranges::find(selection.skill_ids, question.skill_id) == selection.skill_ids.end()) {
    return false;
  }

  if (!selection.kinds.empty() && std::ranges::find(selection.kinds, question.kind) == selection.kinds.end()) {
    return false;
  }

  if (selection.matching_only && !std::holds_alternative<MatchingTask>(question.task)) {
    return false;
  }

  return true;
}

}  // namespace

QuestionBank::QuestionBank(std::vector<Question> questions) : questions_(std::move(questions)) {
  if (questions_.empty()) {
    throw std::invalid_argument("question bank is empty");
  }

  index_.reserve(questions_.size());

  for (std::size_t i = 0; i < questions_.size(); ++i) {
    const auto& id = questions_[i].id;

    const auto [_, inserted] = index_.try_emplace(id, i);

    if (!inserted) {
      throw std::invalid_argument("duplicate question id: " + id);
    }
  }
}

const Question& QuestionBank::Get(std::string_view id) const {
  const auto it = index_.find(std::string(id));

  if (it == index_.end()) {
    throw std::logic_error("unknown question: " + std::string(id));
  }

  return questions_[it->second];
}

std::vector<std::string> QuestionBank::QuestionIds() const {
  std::vector<std::string> ids;
  ids.reserve(questions_.size());

  for (const auto& question : questions_) {
    ids.push_back(question.id);
  }

  return ids;
}

std::vector<std::string> QuestionBank::Select(const QuestionSelection& selection) const {
  ValidateSelection(selection);

  std::vector<std::string> ids;
  ids.reserve(questions_.size());

  for (const auto& question : questions_) {
    if (Matches(question, selection)) {
      ids.push_back(question.id);
    }
  }

  return ids;
}

std::size_t QuestionBank::Size() const noexcept {
  return questions_.size();
}
