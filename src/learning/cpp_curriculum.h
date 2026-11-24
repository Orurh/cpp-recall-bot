#pragma once

#include "learning/progress_summary.h"
#include "learning/review_state.h"
#include "quiz/question_bank.h"

#include <boost/json/object.hpp>

#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

// Read models over the existing question/skill/material catalogs, not another
// learner-progress store. Question identity never depends on its source path.
[[nodiscard]] boost::json::object BuildCppCurriculum(
    const QuestionBank& bank, const UserProgressSummary& progress,
    const std::unordered_map<std::string, QuestionReviewState>& reviews, std::string_view topic);
[[nodiscard]] std::vector<std::string> CppLearningUnitQuestions(
    const QuestionBank& bank, const UserProgressSummary& progress, std::string_view unit_id,
    const std::unordered_map<std::string, QuestionReviewState>& reviews = {});
[[nodiscard]] std::vector<std::string> ResolveCppLearningUnitQuestions(const QuestionBank& bank,
                                                                       std::string_view unit_id);
void ValidateCppCurriculum(const QuestionBank& bank);
