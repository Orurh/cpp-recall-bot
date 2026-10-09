#!/usr/bin/env bash
set -euo pipefail

tmp_patch="$(mktemp)"
trap 'rm -f "$tmp_patch"' EXIT

cat >"$tmp_patch" <<'PATCH'
--- a/src/learning/learning_queue.h
+++ b/src/learning/learning_queue.h
@@ -17,6 +17,8 @@
 
   [[nodiscard]] std::optional<std::string> Current() const;
 
+  [[nodiscard]] std::size_t CurrentAttempts() const;
+
   [[nodiscard]]
   SubmitResult SubmitAnswer(bool correct);
 
--- a/src/learning/learning_queue.cc
+++ b/src/learning/learning_queue.cc
@@ -28,6 +28,15 @@
   }
 
   return pending_.front().question_id;
+}
+
+std::size_t LearningQueue::CurrentAttempts() const {
+  if (pending_.empty()) {
+    throw std::logic_error(
+        "cannot read attempts from an empty queue");
+  }
+
+  return pending_.front().attempts;
 }
 
 SubmitResult LearningQueue::SubmitAnswer(bool correct) {
--- a/src/quiz/session.h
+++ b/src/quiz/session.h
@@ -3,6 +3,7 @@
 #include "learning/learning_queue.h"
 #include "quiz/question_bank.h"
 
+#include <cstddef>
 #include <string>
 #include <vector>
 
@@ -10,6 +11,14 @@
   LearningQueue queue;
   QuestionSelection selection;
   std::vector<std::string> completed_question_ids;
+  std::size_t session_goal = 0;
+  std::size_t total_attempts = 0;
+  std::size_t correct_attempts = 0;
+  std::size_t mastered_questions = 0;
+  std::size_t first_try_correct = 0;
+  std::size_t current_streak = 0;
+  std::size_t best_streak = 0;
+  int initial_level = 3;
   int target_level = 3;
   bool answered = false;
 };
--- a/src/app/quiz_session_service.h
+++ b/src/app/quiz_session_service.h
@@ -62,7 +62,8 @@
   std::vector<std::string> BuildAdaptiveBatch(
       const QuestionSelection& selection,
       int target_level,
-      std::span<const std::string> excluded_ids) const;
+      std::span<const std::string> excluded_ids,
+      std::size_t max_questions) const;
 
   std::reference_wrapper<const QuestionBank> questions_;
 };
--- a/src/app/quiz_session_service.cc
+++ b/src/app/quiz_session_service.cc
@@ -22,6 +22,7 @@
 constexpr int kMaxLevel = 10;
 constexpr int kInitialAdaptiveLevel = 3;
 constexpr std::size_t kAdaptiveBatchSize = 3;
+constexpr std::size_t kSessionQuestionGoal = 10;
 
 void ValidateTargetLevel(int level) {
   if (level < kMinLevel || level > kMaxLevel) {
@@ -69,6 +70,18 @@
           result == AnswerResult::kCorrect);
 
   next.answered = true;
+  ++next.total_attempts;
+
+  if (result == AnswerResult::kCorrect) {
+    ++next.correct_attempts;
+    ++next.current_streak;
+    next.best_streak =
+        std::max(
+            next.best_streak,
+            next.current_streak);
+  } else {
+    next.current_streak = 0;
+  }
 
   if (submit_result.completed) {
     if (std::ranges::find(
@@ -78,6 +91,14 @@
       next.completed_question_ids.push_back(question.id);
     }
 
+    if (result == AnswerResult::kCorrect) {
+      ++next.mastered_questions;
+
+      if (submit_result.attempts == 1) {
+        ++next.first_try_correct;
+      }
+    }
+
     next.target_level = NextTargetLevel(
         question,
         result,
@@ -116,20 +137,37 @@
     int target_level) const {
   ValidateTargetLevel(target_level);
 
+  const auto available_questions =
+      questions_.get().Select(selection);
+
+  if (available_questions.empty()) {
+    throw std::runtime_error(
+        "no questions match selection");
+  }
+
+  const auto session_goal =
+      std::min(
+          kSessionQuestionGoal,
+          available_questions.size());
+
   auto question_ids = BuildAdaptiveBatch(
       selection,
       target_level,
-      {});
-
-  if (question_ids.empty()) {
-    throw std::runtime_error(
-        "no questions match selection");
-  }
+      {},
+      session_goal);
 
   return {
       .queue = LearningQueue(question_ids),
       .selection = selection,
       .completed_question_ids = {},
+      .session_goal = session_goal,
+      .total_attempts = 0,
+      .correct_attempts = 0,
+      .mastered_questions = 0,
+      .first_try_correct = 0,
+      .current_streak = 0,
+      .best_streak = 0,
+      .initial_level = target_level,
       .target_level = target_level,
       .answered = false,
   };
@@ -144,10 +182,21 @@
 
   QuizSession next = session;
 
+  if (next.completed_question_ids.size() >=
+      next.session_goal) {
+    next.answered = false;
+    return next;
+  }
+
+  const auto remaining =
+      next.session_goal -
+      next.completed_question_ids.size();
+
   const auto question_ids = BuildAdaptiveBatch(
       next.selection,
       next.target_level,
-      next.completed_question_ids);
+      next.completed_question_ids,
+      remaining);
 
   next.queue = LearningQueue(question_ids);
   next.answered = false;
@@ -209,7 +258,8 @@
 QuizSessionService::BuildAdaptiveBatch(
     const QuestionSelection& selection,
     int target_level,
-    std::span<const std::string> excluded_ids) const {
+    std::span<const std::string> excluded_ids,
+    std::size_t max_questions) const {
   ValidateTargetLevel(target_level);
 
   auto question_ids =
@@ -245,8 +295,13 @@
         return lhs_distance < rhs_distance;
       });
 
-  if (question_ids.size() > kAdaptiveBatchSize) {
-    question_ids.resize(kAdaptiveBatchSize);
+  const auto batch_size =
+      std::min(
+          kAdaptiveBatchSize,
+          max_questions);
+
+  if (question_ids.size() > batch_size) {
+    question_ids.resize(batch_size);
   }
 
   return question_ids;
--- a/src/bot/telegram_session.h
+++ b/src/bot/telegram_session.h
@@ -2,6 +2,7 @@
 
 #include "quiz/session.h"
 
+#include <cstddef>
 #include <cstdint>
 #include <optional>
 #include <string>
@@ -12,6 +13,8 @@
   QuizSession quiz;
   std::int64_t storage_session_id = 0;
   std::optional<std::int64_t> message_id;
+  std::size_t questions_started = 0;
+  bool current_question_is_repeat = false;
   std::vector<std::string> option_order;
   std::vector<std::string> selected_option_ids;
   std::optional<std::string> pending_explanation_question_id;
--- a/src/bot/card_renderer.h
+++ b/src/bot/card_renderer.h
@@ -6,11 +6,23 @@
 
 #include <boost/json/object.hpp>
 
+#include <cstddef>
 #include <cstdint>
 #include <span>
 #include <string>
 #include <string_view>
 #include <vector>
+
+struct TrainingProgressView {
+  int adaptive_level = 3;
+  std::size_t completed_questions = 0;
+  std::size_t session_goal = 0;
+  std::size_t question_number = 0;
+  std::size_t correct_attempts = 0;
+  std::size_t total_attempts = 0;
+  std::size_t current_streak = 0;
+  bool repeat = false;
+};
 
 boost::json::object BuildStartMenuMessage(
     std::int64_t chat_id,
@@ -24,7 +36,8 @@
     std::int64_t chat_id,
     const Question& question,
     const std::vector<std::string>& option_order,
-    const std::vector<std::string>& selected_option_ids);
+    const std::vector<std::string>& selected_option_ids,
+    const TrainingProgressView& progress);
 
 boost::json::object BuildQuestionKeyboard(
     const Question& question,
@@ -33,7 +46,6 @@
 
 boost::json::object BuildAnswerKeyboard(
     bool show_explanation_button);
-
 
 std::string BuildAnswerDisplayText(
     const Question& question,
@@ -50,4 +62,5 @@
     const std::vector<std::string>& option_order,
     std::string_view answer_display,
     AnswerResult result,
-    bool show_explanation);
+    bool show_explanation,
+    const TrainingProgressView& progress);
--- a/src/bot/card_renderer.cc
+++ b/src/bot/card_renderer.cc
@@ -151,6 +151,86 @@
   }
 }
 
+std::string BuildProgressBar(
+    std::size_t completed,
+    std::size_t goal) {
+  if (goal == 0 || completed > goal) {
+    throw std::invalid_argument(
+        "invalid training progress");
+  }
+
+  std::string result;
+
+  for (std::size_t i = 0; i < goal; ++i) {
+    result += i < completed ? "●" : "○";
+  }
+
+  return result;
+}
+
+std::string BuildTrainingHeader(
+    const TrainingProgressView& progress) {
+  if (progress.session_goal == 0 ||
+      progress.completed_questions >
+          progress.session_goal) {
+    throw std::invalid_argument(
+        "invalid training progress view");
+  }
+
+  std::string text = "<b>Уровень ";
+  text += std::to_string(progress.adaptive_level);
+  text += "</b> · ";
+
+  if (progress.repeat) {
+    text += "Повтор";
+  } else {
+    if (progress.question_number == 0 ||
+        progress.question_number >
+            progress.session_goal) {
+      throw std::invalid_argument(
+          "invalid training question number");
+    }
+
+    text += "Вопрос ";
+    text += std::to_string(
+        progress.question_number);
+    text += '/';
+    text += std::to_string(
+        progress.session_goal);
+  }
+
+  text += "\nПрогресс ";
+  text += BuildProgressBar(
+      progress.completed_questions,
+      progress.session_goal);
+  text += ' ';
+  text += std::to_string(
+      progress.completed_questions);
+  text += '/';
+  text += std::to_string(
+      progress.session_goal);
+
+  text += "\nТочность ";
+
+  if (progress.total_attempts == 0) {
+    text += "—";
+  } else {
+    const auto accuracy =
+        (progress.correct_attempts * 100 +
+         progress.total_attempts / 2) /
+        progress.total_attempts;
+
+    text += std::to_string(accuracy);
+    text += '%';
+  }
+
+  text += " · Серия ";
+  text += std::to_string(
+      progress.current_streak);
+
+  return text;
+}
+
 }  // namespace
 
 boost::json::object BuildStartMenuKeyboard(std::span<const TrainingContentMode> selected_modes) {
@@ -259,8 +339,11 @@
 
 boost::json::object BuildQuestionMessage(std::int64_t chat_id, const Question& question,
                                          const std::vector<std::string>& option_order,
-                                         const std::vector<std::string>& selected_option_ids) {
-  std::string text = EscapeHtml(question.text);
+                                         const std::vector<std::string>& selected_option_ids,
+                                         const TrainingProgressView& progress) {
+  std::string text = BuildTrainingHeader(progress);
+  text += "\n\n";
+  text += EscapeHtml(question.text);
 
   if (question.code) {
     text += "\n\n<pre><code class=\"language-cpp\">";
@@ -348,8 +431,11 @@
 }
 
 std::string BuildAnswerText(const Question& question, const std::vector<std::string>& option_order,
-                            std::string_view answer_display, AnswerResult result, bool show_explanation) {
-  std::string text = EscapeHtml(question.text);
+                            std::string_view answer_display, AnswerResult result, bool show_explanation,
+                            const TrainingProgressView& progress) {
+  std::string text = BuildTrainingHeader(progress);
+  text += "\n\n";
+  text += EscapeHtml(question.text);
 
   if (question.code) {
     text += "\n\n<pre><code class=\"language-cpp\">";
--- a/src/bot/telegram_update_handler.cc
+++ b/src/bot/telegram_update_handler.cc
@@ -7,6 +7,7 @@
 #include <spdlog/spdlog.h>
 
 #include <algorithm>
+#include <cstddef>
 #include <cstdint>
 #include <exception>
 #include <optional>
@@ -185,6 +186,83 @@
   return boost::json::serialize(values);
 }
 
+TrainingProgressView BuildProgressView(
+    const TelegramSession& session) {
+  return {
+      .adaptive_level =
+          session.quiz.target_level,
+      .completed_questions =
+          session.quiz.completed_question_ids.size(),
+      .session_goal =
+          session.quiz.session_goal,
+      .question_number =
+          session.questions_started,
+      .correct_attempts =
+          session.quiz.correct_attempts,
+      .total_attempts =
+          session.quiz.total_attempts,
+      .current_streak =
+          session.quiz.current_streak,
+      .repeat =
+          session.current_question_is_repeat,
+  };
+}
+
+std::string BuildCompletionNotice(
+    const TelegramSession& session) {
+  const auto& quiz = session.quiz;
+
+  std::string text =
+      "🎯 Тренировка завершена.\n\n";
+
+  text += "Пройдено: ";
+  text += std::to_string(
+      quiz.completed_question_ids.size());
+  text += '/';
+  text += std::to_string(quiz.session_goal);
+
+  text += "\nОсвоено: ";
+  text += std::to_string(
+      quiz.mastered_questions);
+  text += '/';
+  text += std::to_string(quiz.session_goal);
+
+  text += "\nС первой попытки: ";
+  text += std::to_string(
+      quiz.first_try_correct);
+  text += '/';
+  text += std::to_string(quiz.session_goal);
+
+  text += "\nТочность: ";
+
+  if (quiz.total_attempts == 0) {
+    text += "—";
+  } else {
+    const auto accuracy =
+        (quiz.correct_attempts * 100 +
+         quiz.total_attempts / 2) /
+        quiz.total_attempts;
+
+    text += std::to_string(accuracy);
+    text += '%';
+  }
+
+  text += "\nЛучшая серия: ";
+  text += std::to_string(quiz.best_streak);
+
+  text += "\nУровень: ";
+  text += std::to_string(quiz.initial_level);
+
+  if (quiz.initial_level !=
+      quiz.target_level) {
+    text += " → ";
+    text += std::to_string(
+        quiz.target_level);
+  }
+
+  return text;
+}
+
 }  // namespace
 
 TelegramUpdateHandler::TelegramUpdateHandler(
@@ -708,7 +786,8 @@
                session.option_order,
                display_answer,
                submission.result,
-               correct)},
+               correct,
+               BuildProgressView(next))},
           {"parse_mode", "HTML"},
           {"reply_markup",
            BuildAnswerKeyboard(
@@ -814,7 +893,8 @@
                session.option_order,
                *session.pending_explanation_answer_text,
                AnswerResult::kIncorrect,
-               true)},
+               true,
+               BuildProgressView(session))},
           {"parse_mode", "HTML"},
           {"reply_markup",
            BuildAnswerKeyboard(false)},
@@ -834,7 +914,7 @@
   attempts_.get().EnsureUser(user_id);
 
   std::string_view finish_reason;
-  std::string_view notice;
+  std::string notice;
 
   switch (action) {
     case MenuAction::kStart:
@@ -868,6 +948,11 @@
 
     previous_message_id =
         session_it->second.message_id;
+
+    if (action == MenuAction::kCompleted) {
+      notice = BuildCompletionNotice(
+          session_it->second);
+    }
 
     FinishStorageSession(
         session_it->second,
@@ -986,6 +1071,8 @@
               storage_mode),
       .message_id =
           menu_it->second.message_id,
+      .questions_started = 0,
+      .current_question_is_repeat = false,
       .option_order = {},
       .selected_option_ids = {},
       .pending_explanation_question_id =
@@ -1026,6 +1113,13 @@
       BuildOptionOrder(question);
 
   session.selected_option_ids.clear();
+
+  session.current_question_is_repeat =
+      session.quiz.queue.CurrentAttempts() > 0;
+
+  if (!session.current_question_is_repeat) {
+    ++session.questions_started;
+  }
 
   const auto new_message_id =
       SendReplacingMessage(
@@ -1035,7 +1129,8 @@
               chat_id,
               question,
               option_order,
-              session.selected_option_ids),
+              session.selected_option_ids,
+              BuildProgressView(session)),
           stop_token);
 
   session.message_id = new_message_id;
PATCH

patch -p1 --dry-run <"$tmp_patch"
patch -p1 <"$tmp_patch"

echo
echo "Session progress patch applied."
echo "Run:"
echo "  make"
echo "  make content-check"
