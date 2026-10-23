#pragma once

#include <cstddef>
#include <optional>
#include <utility>
#include <vector>

enum class MatchingSide { kLeft, kRight };

enum class MatchingPick {
  kIgnored,
  kSelected,
  kMatched,
  kMismatch,
  kFinished,
};

struct MatchingRoundState {
  std::vector<std::size_t> left_order;
  std::vector<std::size_t> right_order;
  std::vector<bool> matched;
  std::optional<std::size_t> selected_left;
  std::optional<std::size_t> selected_right;
  std::size_t mistakes = 0;
  std::size_t revision = 0;
  bool last_mismatch = false;
};

// One presentation of a matching question. Pair indices refer to MatchingTask::pairs.
class MatchingRound {
 public:
  explicit MatchingRound(std::size_t pair_count);
  explicit MatchingRound(MatchingRoundState state);

  [[nodiscard]] MatchingPick Select(MatchingSide side, std::size_t position, std::size_t revision);
  [[nodiscard]] std::size_t PairIndex(MatchingSide side, std::size_t position) const;
  [[nodiscard]] bool IsMatched(std::size_t pair_index) const;
  [[nodiscard]] bool IsSelected(MatchingSide side, std::size_t pair_index) const;
  [[nodiscard]] std::size_t Size() const noexcept;
  [[nodiscard]] std::size_t MatchedCount() const noexcept;
  [[nodiscard]] std::size_t Mistakes() const noexcept;
  [[nodiscard]] std::size_t Revision() const noexcept;
  [[nodiscard]] bool LastMismatch() const noexcept;
  [[nodiscard]] MatchingRoundState State() const;

 private:
  std::vector<std::size_t> left_order_;
  std::vector<std::size_t> right_order_;
  std::vector<bool> matched_;
  std::optional<std::size_t> selected_left_;
  std::optional<std::size_t> selected_right_;
  std::size_t matched_count_ = 0;
  std::size_t mistakes_ = 0;
  std::size_t revision_ = 0;
  bool last_mismatch_ = false;
};

// The completion callback persists the finished answer. The in-memory round is
// replaced only after that callback succeeds, so a storage failure can be
// retried with the same revision.
template <typename OnFinished>
MatchingPick SelectWithCommit(MatchingRound& current, MatchingSide side, std::size_t position, std::size_t revision,
                              OnFinished&& on_finished) {
  auto next = current;
  const auto outcome = next.Select(side, position, revision);
  if (outcome == MatchingPick::kFinished) {
    std::forward<OnFinished>(on_finished)(std::as_const(next));
  }
  current = std::move(next);
  return outcome;
}
