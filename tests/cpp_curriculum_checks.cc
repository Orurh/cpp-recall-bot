#include "learning/cpp_curriculum.h"
#include "storage/question_loader.h"

#include <boost/json.hpp>

#include <algorithm>
#include <stdexcept>

void CheckCppCurriculum() {
  const QuestionBank bank(LoadQuestions("content/questions"));
  ValidateCppCurriculum(bank);
  auto incomplete = LoadQuestions("content/questions");
  std::erase_if(incomplete,
                [](const auto& question) { return question.topic == "containers" && question.level == 10; });
  bool missing_pilot_rejected = false;
  try {
    ValidateCppCurriculum(QuestionBank(std::move(incomplete)));
  } catch (const std::invalid_argument&) {
    missing_pilot_rejected = true;
  }
  if (!missing_pilot_rejected)
    throw std::runtime_error("an incomplete containers pilot passed content validation");
  UserProgressSummary progress;
  const auto catalog = BuildCppCurriculum(bank, progress, {}, "containers");
  if (catalog.at("direction") != "cpp" || catalog.at("levels").as_array().size() != 10)
    throw std::runtime_error("C++ curriculum must retain ten visible levels");
  const auto& first = catalog.at("levels").as_array().front().as_object();
  const auto& last = catalog.at("levels").as_array().back().as_object();
  if (!first.at("unlocked").as_bool() || last.at("unlocked").as_bool() || first.at("completed").as_int64() != 0 ||
      first.at("due").as_int64() != 0)
    throw std::runtime_error("curriculum invented progress or unlocked a hard level");
  bool rejected = false;
  try {
    static_cast<void>(CppLearningUnitQuestions(bank, progress, "cpp.containers.vector.l10"));
  } catch (const std::invalid_argument&) {
    rejected = true;
  }
  if (!rejected)
    throw std::runtime_error("curriculum API bypassed the unlocked ceiling");
  const auto& unit = first.at("units").as_array().front().as_object();
  const auto& question = unit.at("questions").as_array().front().as_object();
  const auto id = std::string(question.at("id").as_string());
  QuestionReviewState review;
  review.correct_count = 1;
  review.due = true;
  const auto practiced = BuildCppCurriculum(bank, progress, {{id, review}}, "containers");
  const auto& level = practiced.at("levels").as_array().front().as_object();
  if (level.at("completed").as_int64() != 1 || level.at("due").as_int64() != 1)
    throw std::runtime_error("curriculum lost old completed/due observations");
  // Looking at metadata must not count as independent mastery or a new answer.
  if (progress.completed_count != 0 || !progress.skills.empty())
    throw std::runtime_error("curriculum observation mutated learner state");
  progress.unlocked_level = 7;
  progress.adaptive_level = 6;
  progress.promotion = {.level = 3, .families = {{"oop.only", true}}};
  const auto separate = BuildCppCurriculum(bank, progress, {}, "containers");
  if (separate.at("promotion_families").as_int64() != 0 || separate.at("working_level").as_int64() != 3 ||
      !separate.at("levels").as_array()[6].as_object().at("unlocked").as_bool())
    throw std::runtime_error("curriculum borrowed another topic's evidence or reclosed historical unlocks");
}
