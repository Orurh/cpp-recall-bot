#pragma once

#include "quiz/question.h"

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

inline constexpr std::string_view kVectorSizeGenerator = "vector_size_v1";
inline constexpr std::string_view kVectorSizeBaseQuestion = "vector_reserve_does_not_change_size";
inline constexpr std::size_t kVectorSizeLimit = 16;

enum class VectorOperationKind { kReserve, kResize, kPushBack, kPopBack, kClear };

struct VectorOperation {
  VectorOperationKind kind;
  int argument = 0;
};

struct VectorSizeStep {
  std::size_t before;
  std::size_t after;
};

struct VectorSizeProgram {
  std::size_t initial_size;
  std::string initialization;
  std::vector<VectorOperation> operations;
};

// Version 1 is immutable: snapshots must remain reproducible across releases.
[[nodiscard]] VectorSizeProgram GenerateVectorSizeProgram(std::uint64_t seed);
[[nodiscard]] std::vector<VectorSizeStep> EvaluateVectorSize(std::size_t initial_size,
                                                             std::span<const VectorOperation> operations);
[[nodiscard]] std::string VectorOperationCode(const VectorOperation& operation);
[[nodiscard]] Question GenerateVectorSizeQuestion(const Question& base, std::uint64_t seed);

struct GeneratedVectorQuestion {
  std::uint64_t seed;
  Question question;
};
