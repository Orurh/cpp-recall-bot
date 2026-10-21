#pragma once

#include "quiz/question.h"

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

struct QuestionSelection {
  std::optional<std::string> topic;
  std::vector<QuestionKind> kinds;
  bool matching_only = false;
  int min_level = 1;
  int max_level = 10;
};

class QuestionBank {
 public:
  explicit QuestionBank(std::vector<Question> questions);

  [[nodiscard]] const Question& Get(std::string_view id) const;
  [[nodiscard]] std::vector<std::string> QuestionIds() const;
  [[nodiscard]] std::vector<std::string> Select(
      const QuestionSelection& selection) const;
  [[nodiscard]] std::size_t Size() const noexcept;

 private:
  std::vector<Question> questions_;
  std::unordered_map<std::string, std::size_t> index_;
};
