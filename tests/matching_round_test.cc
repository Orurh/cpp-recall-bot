#include "quiz/matching_round.h"
#include "app/quiz_session_service.h"
#include "app/training_mode.h"
#include "quiz/question_validator.h"

#include <gtest/gtest.h>

#include <cstddef>
#include <optional>
#include <stdexcept>
#include <string>
#include <variant>
#include <vector>

namespace {

std::size_t PositionOf(const MatchingRound& round, MatchingSide side, std::size_t pair_index) {
  for (std::size_t position = 0; position < round.Size(); ++position) {
    if (round.PairIndex(side, position) == pair_index) {
      return position;
    }
  }
  ADD_FAILURE() << "Pair missing from matching column";
  return round.Size();
}

Question TestQuestion() {
  return {
      .id = "test_matching",
      .topic = "containers",
      .skill_id = "containers.vector",
      .kind = QuestionKind::kApiKnowledge,
      .level = 3,
      .standard = "C++23",
      .tags = {"matching"},
      .text = "Match methods",
      .code = std::nullopt,
      .task = MatchingTask{.pairs =
                               {
                                   {.id = "a", .left = "A", .right = "One"},
                                   {.id = "b", .left = "B", .right = "Two"},
                                   {.id = "c", .left = "C", .right = "Three"},
                               }},
      .explanation = "Explanation",
      .source = std::nullopt,
  };
}

}  // namespace

TEST(MatchingRoundTest, BothColumnsContainEveryPair) {
  MatchingRound round(4);
  for (std::size_t index = 0; index < 4; ++index) {
    EXPECT_LT(PositionOf(round, MatchingSide::kLeft, index), 4U);
    EXPECT_LT(PositionOf(round, MatchingSide::kRight, index), 4U);
    EXPECT_NE(round.PairIndex(MatchingSide::kLeft, index), round.PairIndex(MatchingSide::kRight, index));
  }
}

TEST(MatchingRoundTest, AcceptsRightSideFirstAndIgnoresStaleRevisions) {
  MatchingRound round(3);
  const auto right = PositionOf(round, MatchingSide::kRight, 1);
  const auto left = PositionOf(round, MatchingSide::kLeft, 1);

  EXPECT_EQ(round.Select(MatchingSide::kRight, right, 0), MatchingPick::kSelected);
  EXPECT_EQ(round.Select(MatchingSide::kLeft, left, 0), MatchingPick::kIgnored);
  EXPECT_EQ(round.Select(MatchingSide::kLeft, left, 1), MatchingPick::kMatched);
  EXPECT_EQ(round.MatchedCount(), 1U);
  EXPECT_EQ(round.Mistakes(), 0U);
}

TEST(MatchingRoundTest, WrongPairDoesNotFinishQuestion) {
  MatchingRound round(3);
  const auto left = PositionOf(round, MatchingSide::kLeft, 0);
  const auto wrong_right = PositionOf(round, MatchingSide::kRight, 1);
  EXPECT_EQ(round.Select(MatchingSide::kLeft, left, round.Revision()), MatchingPick::kSelected);
  EXPECT_EQ(round.Select(MatchingSide::kRight, wrong_right, round.Revision()), MatchingPick::kMismatch);
  EXPECT_TRUE(round.LastMismatch());
  EXPECT_EQ(round.Mistakes(), 1U);
  EXPECT_EQ(round.MatchedCount(), 0U);
  EXPECT_FALSE(round.IsSelected(MatchingSide::kLeft, 0));
}

TEST(MatchingRoundTest, LastCorrectPairFinishesRound) {
  MatchingRound round(3);
  for (std::size_t index = 0; index < 3; ++index) {
    EXPECT_EQ(round.Select(MatchingSide::kLeft, PositionOf(round, MatchingSide::kLeft, index), round.Revision()),
              MatchingPick::kSelected);
    EXPECT_EQ(round.Select(MatchingSide::kRight, PositionOf(round, MatchingSide::kRight, index), round.Revision()),
              index == 2 ? MatchingPick::kFinished : MatchingPick::kMatched);
  }
  EXPECT_EQ(round.MatchedCount(), 3U);
  EXPECT_EQ(round.Mistakes(), 0U);
  EXPECT_EQ(round.Select(MatchingSide::kLeft, 0, round.Revision()), MatchingPick::kIgnored);
}

TEST(MatchingRoundTest, MatchingRoundCountsAsOneQuizAttempt) {
  const auto question = TestQuestion();
  ValidateQuestion(question);
  QuestionBank bank({question});
  QuizSessionService quiz(bank);
  const auto session = quiz.Start();

  const auto with_mistakes = quiz.SubmitMatchingAnswer(session, false);
  EXPECT_EQ(with_mistakes.result, AnswerResult::kIncorrect);
  EXPECT_EQ(with_mistakes.attempts, 1U);
  EXPECT_FALSE(with_mistakes.completed);
  EXPECT_EQ(with_mistakes.session.total_attempts, 1U);

  const auto retry = quiz.SubmitMatchingAnswer(with_mistakes.session, true);
  EXPECT_EQ(retry.result, AnswerResult::kCorrect);
  EXPECT_EQ(retry.attempts, 2U);
  EXPECT_TRUE(retry.completed);
  EXPECT_EQ(retry.session.mastered_questions, 1U);
}

TEST(MatchingRoundTest, DuplicateLabelsAreRejected) {
  auto question = TestQuestion();
  auto& pairs = std::get<MatchingTask>(question.task).pairs;
  pairs[1].right = pairs[0].right;
  EXPECT_THROW(ValidateQuestion(question), std::invalid_argument);
}

TEST(QuestionValidationTest, SkillMustExistAndBelongToQuestionTopic) {
  auto question = TestQuestion();
  question.skill_id = "missing.skill";
  EXPECT_THROW(ValidateQuestion(question), std::invalid_argument);

  question.skill_id = "core.scope";
  EXPECT_THROW(ValidateQuestion(question), std::invalid_argument);
}

TEST(MatchingModeTest, FiltersOtherTaskTypesAndKeepsAllMatchingKinds) {
  auto api_matching = TestQuestion();
  auto theory_matching = TestQuestion();
  theory_matching.id = "sorting_matching";
  theory_matching.kind = QuestionKind::kTheory;
  auto ordinary = TestQuestion();
  ordinary.id = "ordinary_choice";
  ordinary.task =
      SingleChoiceTask{.options = {{.id = "a", .text = "Yes"}, {.id = "b", .text = "No"}}, .correct_option_id = "a"};
  QuestionBank bank({api_matching, theory_matching, ordinary});

  const std::vector modes{TrainingContentMode::kMatching};
  const auto selection = QuestionSelectionForModes(modes);
  EXPECT_TRUE(selection.matching_only);
  EXPECT_TRUE(selection.kinds.empty());
  EXPECT_EQ(TrainingModeStorageName(modes), "training:matching");
  EXPECT_EQ(bank.Select(selection).size(), 2U);

  QuizSessionService quiz(bank);
  const auto session = quiz.Start(selection, 3);
  EXPECT_EQ(session.session_goal, 2U);
  EXPECT_TRUE(std::holds_alternative<MatchingTask>(quiz.CurrentQuestion(session).task));
}
