#pragma once

#include <cstddef>
#include <deque>
#include <optional>
#include <string>
#include <vector>

struct SubmitResult {
  std::size_t attempts = 0;
  bool completed = false;
};

class LearningQueue {
 public:
  explicit LearningQueue(const std::vector<std::string>& question_ids);

  [[nodiscard]] std::optional<std::string> Current() const;

  [[nodiscard]] std::size_t CurrentAttempts() const;

  [[nodiscard]]
  SubmitResult SubmitAnswer(bool correct);

  [[nodiscard]] bool Empty() const noexcept;

 private:
  struct Entry {
    std::string question_id;
    std::size_t attempts = 0;
  };

  std::deque<Entry> pending_;
};
