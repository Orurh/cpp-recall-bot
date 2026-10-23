#include "learning_queue.h"

#include <algorithm>
#include <cstddef>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

constexpr std::size_t kMaximumTrackedAttempts = 1'000;

bool ContainsQuestion(const std::deque<LearningQueue::Entry>& entries, std::string_view question_id) {
  return std::ranges::find(entries, question_id, &LearningQueue::Entry::question_id) != entries.end();
}

}  // namespace

LearningQueue::LearningQueue(const std::vector<std::string>& question_ids) {
  AppendPrimary(question_ids);
}

LearningQueue::LearningQueue(std::vector<Entry> pending, std::vector<Entry> repairs, RestoreStateTag) {
  for (auto& entry : pending) {
    if (entry.question_id.empty() || entry.attempts != 0 || ContainsQuestion(pending_, entry.question_id)) {
      throw std::invalid_argument("invalid persisted primary learning queue entry");
    }
    pending_.push_back(std::move(entry));
  }
  for (auto& entry : repairs) {
    if (entry.question_id.empty() || entry.attempts == 0 || entry.attempts > kMaximumTrackedAttempts ||
        ContainsQuestion(pending_, entry.question_id) || ContainsQuestion(repairs_, entry.question_id)) {
      throw std::invalid_argument("invalid persisted repair learning queue entry");
    }
    repairs_.push_back(std::move(entry));
  }
}

std::optional<std::string> LearningQueue::Current() const {
  if (!pending_.empty()) {
    return pending_.front().question_id;
  }
  return repairs_.empty() ? std::nullopt : std::optional<std::string>(repairs_.front().question_id);
}

std::size_t LearningQueue::CurrentAttempts() const {
  if (!pending_.empty()) {
    return pending_.front().attempts;
  }
  if (repairs_.empty()) {
    throw std::logic_error("cannot read attempts from an empty queue");
  }
  return repairs_.front().attempts;
}

SubmitResult LearningQueue::SubmitAnswer(bool correct) {
  if (Empty()) {
    throw std::logic_error("cannot submit answer when the queue is empty");
  }

  const bool primary = !pending_.empty();
  auto& active = primary ? pending_ : repairs_;
  auto current_entry = std::move(active.front());
  active.pop_front();

  ++current_entry.attempts;
  if (current_entry.attempts > kMaximumTrackedAttempts) {
    throw std::logic_error("question exceeded the maximum number of tracked attempts");
  }

  const auto attempts = current_entry.attempts;
  if (!correct) {
    repairs_.push_back(std::move(current_entry));
  }

  return {
      .attempts = attempts,
      .completed = correct,
  };
}

bool LearningQueue::Empty() const noexcept {
  return pending_.empty() && repairs_.empty();
}

bool LearningQueue::PrimaryEmpty() const noexcept {
  return pending_.empty();
}

const std::deque<LearningQueue::Entry>& LearningQueue::PrimaryPending() const noexcept {
  return pending_;
}

const std::deque<LearningQueue::Entry>& LearningQueue::RepairPending() const noexcept {
  return repairs_;
}

std::vector<std::string> LearningQueue::ScheduledQuestionIds() const {
  std::vector<std::string> result;
  result.reserve(pending_.size() + repairs_.size());
  for (const auto& entry : pending_)
    result.push_back(entry.question_id);
  for (const auto& entry : repairs_)
    result.push_back(entry.question_id);
  return result;
}

void LearningQueue::AppendPrimary(const std::vector<std::string>& question_ids) {
  for (const auto& question_id : question_ids) {
    if (question_id.empty() || ContainsQuestion(pending_, question_id) || ContainsQuestion(repairs_, question_id)) {
      throw std::invalid_argument("question id added to a learning queue must be non-empty and unique");
    }
    pending_.push_back({.question_id = question_id});
  }
}
