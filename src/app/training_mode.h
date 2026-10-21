#pragma once

#include "quiz/question_bank.h"

#include <algorithm>
#include <cstdint>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

enum class TrainingContentMode : std::uint8_t {
  kTheory,
  kApi,
  kCode,
  kMatching,
};

inline bool ContainsTrainingMode(
    std::span<const TrainingContentMode> modes,
    TrainingContentMode mode) {
  return std::ranges::find(modes, mode) !=
         modes.end();
}

inline QuestionSelection QuestionSelectionForModes(
    std::span<const TrainingContentMode> modes) {
  if (modes.empty()) {
    throw std::invalid_argument(
        "at least one training mode must be selected");
  }

  QuestionSelection selection;

  if (ContainsTrainingMode(modes, TrainingContentMode::kMatching)) {
    if (modes.size() != 1) {
      throw std::invalid_argument(
          "matching mode cannot be combined with other modes");
    }
    selection.matching_only = true;
    return selection;
  }

  const auto add_kind =
      [&selection](QuestionKind kind) {
        if (std::ranges::find(
                selection.kinds,
                kind) ==
            selection.kinds.end()) {
          selection.kinds.push_back(kind);
        }
      };

  for (const auto mode : modes) {
    switch (mode) {
      case TrainingContentMode::kTheory:
        add_kind(QuestionKind::kTheory);
        break;

      case TrainingContentMode::kApi:
        add_kind(QuestionKind::kApiKnowledge);
        break;

      case TrainingContentMode::kCode:
        add_kind(QuestionKind::kCodeReading);
        add_kind(QuestionKind::kDebugging);
        add_kind(QuestionKind::kImplementation);
        break;
      case TrainingContentMode::kMatching:
        // Handled as an exclusive mode before the switch.
        break;
    }
  }

  return selection;
}

inline std::string TrainingModeStorageName(
    std::span<const TrainingContentMode> modes) {
  if (modes.empty()) {
    throw std::invalid_argument(
        "at least one training mode must be selected");
  }

  if (ContainsTrainingMode(modes, TrainingContentMode::kMatching)) {
    if (modes.size() != 1) {
      throw std::invalid_argument(
          "matching mode cannot be combined with other modes");
    }
    return "training:matching";
  }

  std::string result = "training:";

  const auto append =
      [&result](std::string_view name) {
        if (result.back() != ':') {
          result += '+';
        }
        result += name;
      };

  if (ContainsTrainingMode(
          modes,
          TrainingContentMode::kTheory)) {
    append("theory");
  }

  if (ContainsTrainingMode(
          modes,
          TrainingContentMode::kApi)) {
    append("api");
  }

  if (ContainsTrainingMode(
          modes,
          TrainingContentMode::kCode)) {
    append("code");
  }

  return result;
}
