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
  struct Entry {
    std::string question_id;
    std::size_t attempts = 0;
  };

  struct RestoreStateTag {};

  explicit LearningQueue(const std::vector<std::string>& question_ids);
  LearningQueue(std::vector<Entry> pending, std::vector<Entry> repairs, RestoreStateTag);

  [[nodiscard]] std::optional<std::string> Current() const;

  [[nodiscard]] std::size_t CurrentAttempts() const;

  [[nodiscard]]
  SubmitResult SubmitAnswer(bool correct);

  [[nodiscard]] bool Empty() const noexcept;

  [[nodiscard]] bool PrimaryEmpty() const noexcept;

  [[nodiscard]] const std::deque<Entry>& PrimaryPending() const noexcept;

  [[nodiscard]] const std::deque<Entry>& RepairPending() const noexcept;

  [[nodiscard]] std::vector<std::string> ScheduledQuestionIds() const;

  void AppendPrimary(const std::vector<std::string>& question_ids);

 private:
  std::deque<Entry> pending_;
  std::deque<Entry> repairs_;
};
