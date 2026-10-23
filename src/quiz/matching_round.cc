#include "matching_round.h"

#include <algorithm>
#include <numeric>
#include <random>
#include <stdexcept>
#include <utility>

MatchingRound::MatchingRound(std::size_t pair_count)
    : left_order_(pair_count), right_order_(pair_count), matched_(pair_count, false) {
  if (pair_count < 2 || pair_count > 6) {
    throw std::invalid_argument("matching task requires 2 to 6 pairs");
  }

  std::iota(left_order_.begin(), left_order_.end(), std::size_t{0});
  std::iota(right_order_.begin(), right_order_.end(), std::size_t{0});

  static thread_local std::mt19937 generator{std::random_device{}()};
  std::ranges::shuffle(left_order_, generator);
  std::ranges::shuffle(right_order_, generator);

  // Avoid revealing any pair by putting its halves in the same row.
  for (std::size_t i = 0; i < pair_count; ++i) {
    if (left_order_[i] == right_order_[i]) {
      std::swap(right_order_[i], right_order_[(i + 1) % pair_count]);
    }
  }
}

MatchingRound::MatchingRound(MatchingRoundState state)
    : left_order_(std::move(state.left_order)),
      right_order_(std::move(state.right_order)),
      matched_(std::move(state.matched)),
      selected_left_(state.selected_left),
      selected_right_(state.selected_right),
      matched_count_(static_cast<std::size_t>(std::ranges::count(matched_, true))),
      mistakes_(state.mistakes),
      revision_(state.revision),
      last_mismatch_(state.last_mismatch) {
  const auto pair_count = matched_.size();
  if (pair_count < 2 || pair_count > 6 || left_order_.size() != pair_count || right_order_.size() != pair_count) {
    throw std::invalid_argument("invalid persisted matching round size");
  }

  const auto valid_order = [pair_count](const std::vector<std::size_t>& order) {
    auto sorted = order;
    std::ranges::sort(sorted);
    for (std::size_t index = 0; index < pair_count; ++index) {
      if (sorted[index] != index) {
        return false;
      }
    }
    return true;
  };
  if (!valid_order(left_order_) || !valid_order(right_order_)) {
    throw std::invalid_argument("persisted matching order is not a permutation");
  }
  if (selected_left_ && (*selected_left_ >= pair_count || matched_[*selected_left_])) {
    throw std::invalid_argument("invalid persisted left matching selection");
  }
  if (selected_right_ && (*selected_right_ >= pair_count || matched_[*selected_right_])) {
    throw std::invalid_argument("invalid persisted right matching selection");
  }
  if (selected_left_ && selected_right_) {
    throw std::invalid_argument("persisted matching round has two pending selections");
  }
}

std::size_t MatchingRound::PairIndex(MatchingSide side, std::size_t position) const {
  const auto& order = side == MatchingSide::kLeft ? left_order_ : right_order_;
  if (position >= order.size()) {
    throw std::out_of_range("matching position outside column");
  }
  return order[position];
}

bool MatchingRound::IsMatched(std::size_t pair_index) const {
  return matched_.at(pair_index);
}

bool MatchingRound::IsSelected(MatchingSide side, std::size_t pair_index) const {
  return side == MatchingSide::kLeft ? selected_left_ == pair_index : selected_right_ == pair_index;
}

std::size_t MatchingRound::Size() const noexcept {
  return matched_.size();
}
std::size_t MatchingRound::MatchedCount() const noexcept {
  return matched_count_;
}
std::size_t MatchingRound::Mistakes() const noexcept {
  return mistakes_;
}
std::size_t MatchingRound::Revision() const noexcept {
  return revision_;
}
bool MatchingRound::LastMismatch() const noexcept {
  return last_mismatch_;
}

MatchingRoundState MatchingRound::State() const {
  return {
      .left_order = left_order_,
      .right_order = right_order_,
      .matched = matched_,
      .selected_left = selected_left_,
      .selected_right = selected_right_,
      .mistakes = mistakes_,
      .revision = revision_,
      .last_mismatch = last_mismatch_,
  };
}

MatchingPick MatchingRound::Select(MatchingSide side, std::size_t position, std::size_t revision) {
  if (revision != revision_ || position >= matched_.size() || matched_count_ == matched_.size()) {
    return MatchingPick::kIgnored;
  }

  const auto pair_index = PairIndex(side, position);
  if (matched_[pair_index]) {
    return MatchingPick::kIgnored;
  }

  // A selection can start on either side. Selecting within the same column
  // changes (or clears) that selection without counting a mistake.
  auto& own = side == MatchingSide::kLeft ? selected_left_ : selected_right_;
  auto& other = side == MatchingSide::kLeft ? selected_right_ : selected_left_;

  ++revision_;
  last_mismatch_ = false;

  if (!other) {
    if (own == pair_index) {
      own.reset();
    } else {
      own = pair_index;
    }
    return MatchingPick::kSelected;
  }

  if (*other != pair_index) {
    ++mistakes_;
    last_mismatch_ = true;
    own.reset();
    other.reset();
    return MatchingPick::kMismatch;
  }

  matched_[pair_index] = true;
  ++matched_count_;
  own.reset();
  other.reset();
  return matched_count_ == matched_.size() ? MatchingPick::kFinished : MatchingPick::kMatched;
}
