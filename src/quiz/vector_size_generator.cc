#include "quiz/vector_size_generator.h"

#include "quiz/question_identity.h"

#include <boost/json.hpp>

#include <algorithm>
#include <array>
#include <stdexcept>

namespace {

class Random {
 public:
  explicit Random(std::uint64_t seed) : state_(seed) {}
  std::uint64_t Next() {
    // Defined unsigned arithmetic, independent of standard-library distributions.
    auto value = (state_ += 0x9e3779b97f4a7c15ULL);
    value = (value ^ (value >> 30)) * 0xbf58476d1ce4e5b9ULL;
    value = (value ^ (value >> 27)) * 0x94d049bb133111ebULL;
    return value ^ (value >> 31);
  }

 private:
  std::uint64_t state_;
};

constexpr std::array kKinds{VectorOperationKind::kReserve, VectorOperationKind::kResize, VectorOperationKind::kPushBack,
                            VectorOperationKind::kPopBack, VectorOperationKind::kClear};

}  // namespace

std::vector<VectorSizeStep> EvaluateVectorSize(std::size_t size, std::span<const VectorOperation> operations) {
  if (size > kVectorSizeLimit || operations.size() > 5)
    throw std::invalid_argument("vector model bounds exceeded");
  std::vector<VectorSizeStep> steps;
  for (const auto& operation : operations) {
    const auto before = size;
    switch (operation.kind) {
      case VectorOperationKind::kReserve:
        if (operation.argument < 0 || operation.argument > static_cast<int>(kVectorSizeLimit))
          throw std::invalid_argument("invalid reserve argument");
        break;
      case VectorOperationKind::kResize:
        if (operation.argument < 0 || operation.argument > static_cast<int>(kVectorSizeLimit))
          throw std::invalid_argument("invalid resize argument");
        size = static_cast<std::size_t>(operation.argument);
        break;
      case VectorOperationKind::kPushBack:
        if (size == kVectorSizeLimit)
          throw std::invalid_argument("vector model size limit exceeded");
        ++size;
        break;
      case VectorOperationKind::kPopBack:
        if (size == 0)
          throw std::invalid_argument("pop_back on empty vector");
        --size;
        break;
      case VectorOperationKind::kClear:
        size = 0;
        break;
      default:
        throw std::invalid_argument("unknown vector operation");
    }
    steps.push_back({before, size});
  }
  return steps;
}

VectorSizeProgram GenerateVectorSizeProgram(std::uint64_t seed) {
  Random random(seed);
  const auto initial_size = static_cast<std::size_t>(random.Next() % 5);
  VectorSizeProgram result{initial_size, {}, {}};
  if (random.Next() % 2 == 0) {
    result.initialization = "std::vector<int> v{";
    for (std::size_t index = 0; index < initial_size; ++index) {
      if (index != 0)
        result.initialization += ", ";
      result.initialization += std::to_string(random.Next() % 30);
    }
    result.initialization += "};";
  } else {
    result.initialization =
        "std::vector<int> v(" + std::to_string(initial_size) + ", " + std::to_string(random.Next() % 30) + ");";
  }
  const auto count = 3 + random.Next() % 3;
  std::size_t guard_size = initial_size;
  bool has_reserve = false;
  for (std::uint64_t index = 0; index < count; ++index) {
    std::vector<VectorOperationKind> allowed;
    for (const auto kind : kKinds) {
      if ((!result.operations.empty() && result.operations.back().kind == kind) ||
          (kind == VectorOperationKind::kPopBack && guard_size == 0) ||
          (kind == VectorOperationKind::kPushBack && guard_size == kVectorSizeLimit))
        continue;
      allowed.push_back(kind);
    }
    const auto kind =
        index + 1 == count && !has_reserve ? VectorOperationKind::kReserve : allowed[random.Next() % allowed.size()];
    const int argument = kind == VectorOperationKind::kReserve    ? static_cast<int>(random.Next() % 17)
                         : kind == VectorOperationKind::kResize   ? static_cast<int>(random.Next() % 9)
                         : kind == VectorOperationKind::kPushBack ? static_cast<int>(random.Next() % 51)
                                                                  : 0;
    result.operations.push_back({kind, argument});
    // Generation guards only. The answer and trace are computed by the separate model.
    if (kind == VectorOperationKind::kResize)
      guard_size = static_cast<std::size_t>(argument);
    if (kind == VectorOperationKind::kPushBack)
      ++guard_size;
    if (kind == VectorOperationKind::kPopBack)
      --guard_size;
    if (kind == VectorOperationKind::kClear)
      guard_size = 0;
    has_reserve = has_reserve || kind == VectorOperationKind::kReserve;
  }
  return result;
}

std::string VectorOperationCode(const VectorOperation& operation) {
  switch (operation.kind) {
    case VectorOperationKind::kReserve:
      return "v.reserve(" + std::to_string(operation.argument) + ");";
    case VectorOperationKind::kResize:
      return "v.resize(" + std::to_string(operation.argument) + ");";
    case VectorOperationKind::kPushBack:
      return "v.push_back(" + std::to_string(operation.argument) + ");";
    case VectorOperationKind::kPopBack:
      return "v.pop_back();";
    case VectorOperationKind::kClear:
      return "v.clear();";
  }
  throw std::invalid_argument("unknown vector operation");
}

Question GenerateVectorSizeQuestion(const Question& base, std::uint64_t seed) {
  if (base.id != kVectorSizeBaseQuestion || base.skill_id != "containers.vector" || base.transfer ||
      base.evidence_type != QuestionEvidenceType::kApplication)
    throw std::invalid_argument("invalid vector generator base");
  const auto program = GenerateVectorSizeProgram(seed);
  const auto steps = EvaluateVectorSize(program.initial_size, program.operations);
  Question question = base;
  question.standard = "C++23";
  question.kind = QuestionKind::kCodeReading;
  question.tags = {"vector", "size", "generated_vector_size", "numeric_answer"};
  question.text = "Чему равен v.size() после успешного выполнения программы?";
  question.code = "#include <iostream>\n#include <vector>\n\nint main() {\n  " + program.initialization + "\n";
  question.explanation = "После создания вектора size() = " + std::to_string(program.initial_size) + ".\n\n";
  for (std::size_t index = 0; index < program.operations.size(); ++index) {
    const auto& operation = program.operations[index];
    const auto& step = steps[index];
    const auto code = VectorOperationCode(operation);
    *question.code += "  " + code + "\n";
    question.explanation += std::to_string(index + 1) + ". " + code + " size(): " + std::to_string(step.before) +
                            " → " + std::to_string(step.after) + ". ";
    switch (operation.kind) {
      case VectorOperationKind::kReserve:
        question.explanation += "Запрос ёмкости не создаёт и не удаляет элементы; точная capacity() не задана.";
        break;
      case VectorOperationKind::kResize:
        question.explanation += step.after > step.before   ? "Добавлены элементы int со значением 0."
                                : step.after < step.before ? "Удалён хвост вектора."
                                                           : "Размер уже равен запрошенному.";
        break;
      case VectorOperationKind::kPushBack:
        question.explanation += "В конец добавлен один элемент. Аргумент задаёт его значение, а не требуемый размер.";
        break;
      case VectorOperationKind::kPopBack:
        question.explanation += "Вектор непустой: удалён последний элемент.";
        break;
      case VectorOperationKind::kClear:
        question.explanation += "Все элементы удалены; ёмкость не меняется.";
        break;
    }
    question.explanation += "\n\n";
  }
  *question.code += "  std::cout << v.size();\n}\n";
  const auto answer = std::to_string(steps.back().after);
  question.task = ShortAnswerTask{{answer}};
  question.explanation +=
      "Итоговый размер: " + answer +
      ". Основание C++23/N4950: [vector.capacity]/4,6,15 и [sequence.reqmts]/54–55,100,104,115–116.";
  question.source = "https://timsong-cpp.github.io/cppwp/n4950/vector.capacity";
  auto content = boost::json::parse(base.content_snapshot_json).as_object();
  content["standard"] = question.standard;
  content["kind"] = "code_reading";
  content["text"] = question.text;
  content["code"] = *question.code;
  content["tags"] = boost::json::array{"vector", "size", "generated_vector_size", "numeric_answer"};
  content["task"] = boost::json::object{{"type", "short_answer"}, {"accepted_answers", boost::json::array{answer}}};
  content["explanation"] = question.explanation;
  content["source"] = *question.source;
  content["generator"] = boost::json::object{{"id", kVectorSizeGenerator}, {"seed", std::to_string(seed)}};
  question.content_snapshot_json = CanonicalQuestionJson(content);
  question.content_revision = QuestionContentRevision(question.content_snapshot_json);
  return question;
}
