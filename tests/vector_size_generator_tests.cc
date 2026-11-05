#include "quiz/vector_size_generator.h"

#include <boost/json.hpp>

#include <iostream>
#include <set>
#include <stdexcept>

namespace {
void Check(bool value, const char* message) {
  if (!value)
    throw std::runtime_error(message);
}
void CheckModel(std::uint64_t seed) {
  const auto program = GenerateVectorSizeProgram(seed);
  const auto second = GenerateVectorSizeProgram(seed);
  Check(program.initialization == second.initialization && program.operations.size() == second.operations.size(),
        "generation is not reproducible");
  Check(program.operations.size() >= 3 && program.operations.size() <= 5, "invalid operation count");
  const auto steps = EvaluateVectorSize(program.initial_size, program.operations);
  std::vector<int> actual(program.initial_size);
  bool reserve = false;
  for (std::size_t index = 0; index < program.operations.size(); ++index) {
    const auto operation = program.operations[index];
    Check(VectorOperationCode(operation) == VectorOperationCode(second.operations[index]), "operation drift");
    Check(steps[index].before == actual.size(), "incorrect trace input");
    switch (operation.kind) {
      case VectorOperationKind::kReserve:
        actual.reserve(static_cast<std::size_t>(operation.argument));
        reserve = true;
        break;
      case VectorOperationKind::kResize:
        actual.resize(static_cast<std::size_t>(operation.argument));
        break;
      case VectorOperationKind::kPushBack:
        actual.push_back(operation.argument);
        break;
      case VectorOperationKind::kPopBack:
        Check(!actual.empty(), "generated pop_back on empty vector");
        actual.pop_back();
        break;
      case VectorOperationKind::kClear:
        actual.clear();
        break;
    }
    Check(steps[index].after == actual.size() && actual.size() <= kVectorSizeLimit, "model differs from std::vector");
  }
  Check(reserve, "generated exercise does not test reserve/size family");
}

void Emit() {
  boost::json::array samples;
  for (std::uint64_t seed = 0; seed < 512; ++seed) {
    const auto program = GenerateVectorSizeProgram(seed);
    const auto steps = EvaluateVectorSize(program.initial_size, program.operations);
    boost::json::array operations, expected;
    expected.emplace_back(program.initial_size);
    for (std::size_t index = 0; index < steps.size(); ++index) {
      operations.emplace_back(VectorOperationCode(program.operations[index]));
      expected.emplace_back(steps[index].after);
    }
    samples.emplace_back(boost::json::object{{"initialization", program.initialization},
                                             {"operations", std::move(operations)},
                                             {"expected", std::move(expected)}});
  }
  std::cout << boost::json::serialize(samples);
}
}  // namespace

int main(int argc, char**) {
  try {
    if (argc > 1) {
      Emit();
      return 0;
    }
    std::set<std::string> structures, initializations;
    std::set<std::size_t> answers;
    std::set<int> kinds;
    bool grows = false, shrinks = false, same = false, reserve_below_size = false;
    for (std::uint64_t seed = 0; seed < 50'000; ++seed) {
      CheckModel(seed);
      const auto program = GenerateVectorSizeProgram(seed);
      const auto steps = EvaluateVectorSize(program.initial_size, program.operations);
      initializations.insert(program.initialization);
      std::string structure;
      for (std::size_t i = 0; i < steps.size(); ++i) {
        const auto operation = program.operations[i];
        structure += std::to_string(static_cast<int>(operation.kind));
        kinds.insert(static_cast<int>(operation.kind));
        if (operation.kind == VectorOperationKind::kResize) {
          grows = grows || steps[i].after > steps[i].before;
          shrinks = shrinks || steps[i].after < steps[i].before;
          same = same || steps[i].after == steps[i].before;
        }
        reserve_below_size = reserve_below_size || (operation.kind == VectorOperationKind::kReserve &&
                                                    static_cast<std::size_t>(operation.argument) < steps[i].before);
      }
      structures.insert(structure);
      answers.insert(steps.back().after);
    }
    CheckModel(UINT64_MAX);
    Check(structures.size() > 100 && initializations.size() > 100 && answers.size() >= 9 && kinds.size() == 5 &&
              grows && shrinks && same && reserve_below_size,
          "insufficient structural/boundary diversity");
    for (const auto operation :
         {VectorOperation{VectorOperationKind::kPopBack}, VectorOperation{VectorOperationKind::kResize, -1},
          VectorOperation{VectorOperationKind::kReserve, 17}, VectorOperation{static_cast<VectorOperationKind>(99)}}) {
      bool rejected = false;
      try {
        static_cast<void>(EvaluateVectorSize(0, std::span(&operation, 1)));
      } catch (const std::invalid_argument&) {
        rejected = true;
      }
      Check(rejected, "invalid model input accepted");
    }
    std::cout << "PASS: 50000 seeds + UINT64_MAX; structures=" << structures.size() << "; answers=" << answers.size()
              << "; all traces match std::vector\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
