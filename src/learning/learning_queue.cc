#include "learning_queue.h"

#include <cstddef>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {

constexpr std::size_t kMaxAttempts = 3;

}  // namespace

LearningQueue::LearningQueue(const std::vector<std::string>& question_ids) {
  for (const auto& question_id : question_ids) {
    pending_.push_back({
        .question_id = question_id,
    });
  }
}

std::optional<std::string> LearningQueue::Current() const {
  if (pending_.empty()) {
    return std::nullopt;
  }

  return pending_.front().question_id;
}

std::size_t LearningQueue::CurrentAttempts() const {
  if (pending_.empty()) {
    throw std::logic_error("cannot read attempts from an empty queue");
  }

  return pending_.front().attempts;
}

SubmitResult LearningQueue::SubmitAnswer(bool correct) {
  if (Empty()) {
    throw std::logic_error("cannot submit answer when the queue is empty");
  }

  auto current_entry = std::move(pending_.front());

  pending_.pop_front();

  ++current_entry.attempts;

  const auto attempts = current_entry.attempts;

  const bool completed = correct || attempts >= kMaxAttempts;

  if (!completed) {
    pending_.push_back(std::move(current_entry));
  }

  return {
      .attempts = attempts,
      .completed = completed,
  };
}

bool LearningQueue::Empty() const noexcept {
  return pending_.empty();
}
