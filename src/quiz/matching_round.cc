#include "matching_round.h"

#include <algorithm>
#include <numeric>
#include <random>
#include <stdexcept>

MatchingRound::MatchingRound(std::size_t pair_count)
    : left_order_(pair_count),
      right_order_(pair_count),
      matched_(pair_count, false) {
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

std::size_t MatchingRound::PairIndex(
    MatchingSide side, std::size_t position) const {
  const auto& order = side == MatchingSide::kLeft
                          ? left_order_ : right_order_;
  if (position >= order.size()) {
    throw std::out_of_range("matching position outside column");
  }
  return order[position];
}

bool MatchingRound::IsMatched(std::size_t pair_index) const {
  return matched_.at(pair_index);
}

bool MatchingRound::IsSelected(
    MatchingSide side, std::size_t pair_index) const {
  return side == MatchingSide::kLeft
             ? selected_left_ == pair_index
             : selected_right_ == pair_index;
}

std::size_t MatchingRound::Size() const noexcept { return matched_.size(); }
std::size_t MatchingRound::MatchedCount() const noexcept { return matched_count_; }
std::size_t MatchingRound::Mistakes() const noexcept { return mistakes_; }
std::size_t MatchingRound::Revision() const noexcept { return revision_; }
bool MatchingRound::LastMismatch() const noexcept { return last_mismatch_; }

MatchingPick MatchingRound::Select(
    MatchingSide side, std::size_t position, std::size_t revision) {
  if (revision != revision_ || position >= matched_.size() ||
      matched_count_ == matched_.size()) {
    return MatchingPick::kIgnored;
  }

  const auto pair_index = PairIndex(side, position);
  if (matched_[pair_index]) {
    return MatchingPick::kIgnored;
  }

  // A selection can start on either side. Selecting within the same column
  // changes (or clears) that selection without counting a mistake.
  auto& own = side == MatchingSide::kLeft
                  ? selected_left_ : selected_right_;
  auto& other = side == MatchingSide::kLeft
                    ? selected_right_ : selected_left_;

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
  return matched_count_ == matched_.size()
             ? MatchingPick::kFinished : MatchingPick::kMatched;
}
