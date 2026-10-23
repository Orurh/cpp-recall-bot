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
    if (question_id.empty()) {
      throw std::invalid_argument("question id in a learning queue must not be empty");
    }
    pending_.push_back({
        .question_id = question_id,
    });
  }
}

LearningQueue::LearningQueue(std::vector<Entry> pending, RestoreStateTag) {
  for (auto& entry : pending) {
    if (entry.question_id.empty() || entry.attempts >= kMaxAttempts) {
      throw std::invalid_argument("invalid persisted learning queue entry");
    }
    pending_.push_back(std::move(entry));
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

const std::deque<LearningQueue::Entry>& LearningQueue::Pending() const noexcept {
  return pending_;
}
