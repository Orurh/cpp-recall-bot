#!/usr/bin/env bash
set -euo pipefail

tmp_patch="$(mktemp)"
trap 'rm -f "$tmp_patch"' EXIT

cat >"$tmp_patch" <<'PATCH'
diff -ruN a/src/app/quiz_session_service.cc b/src/app/quiz_session_service.cc
--- a/src/app/quiz_session_service.cc	2026-10-06 20:03:00.800493151 +0000
+++ b/src/app/quiz_session_service.cc	2026-10-06 20:05:26.310139659 +0000
@@ -13,6 +13,7 @@
 #include <stdexcept>
 #include <string>
 #include <string_view>
+#include <unordered_map>
 #include <utility>
 #include <vector>
 
@@ -35,6 +36,22 @@
   return lhs >= rhs ? lhs - rhs : rhs - lhs;
 }
 
+int TopicLevelOrFallback(
+    const std::unordered_map<std::string, int>& topic_levels,
+    std::string_view topic,
+    int fallback_level) {
+  ValidateTargetLevel(fallback_level);
+
+  const auto it = topic_levels.find(std::string(topic));
+
+  if (it == topic_levels.end()) {
+    return fallback_level;
+  }
+
+  ValidateTargetLevel(it->second);
+  return it->second;
+}
+
 int NextTargetLevel(
     int current_level,
     const Question& question,
@@ -110,6 +127,20 @@
       }
     }
 
+    const auto current_topic_level =
+        TopicLevelOrFallback(
+            session.topic_levels,
+            question.topic,
+            session.target_level);
+
+    next.topic_levels.insert_or_assign(
+        question.topic,
+        NextTargetLevel(
+            current_topic_level,
+            question,
+            result,
+            submit_result.attempts));
+
     next.target_level = NextTargetLevel(
         session.target_level,
         question,
@@ -147,8 +178,27 @@
 QuizSession QuizSessionService::Start(
     const QuestionSelection& selection,
     int target_level) const {
+  return Start(
+      selection,
+      target_level,
+      {});
+}
+
+QuizSession QuizSessionService::Start(
+    const QuestionSelection& selection,
+    int target_level,
+    std::unordered_map<std::string, int> topic_levels) const {
   ValidateTargetLevel(target_level);
 
+  for (const auto& [topic, level] : topic_levels) {
+    if (topic.empty()) {
+      throw std::invalid_argument(
+          "topic level key must not be empty");
+    }
+
+    ValidateTargetLevel(level);
+  }
+
   const auto available_questions =
       questions_.get().Select(selection);
 
@@ -165,6 +215,7 @@
   auto question_ids = BuildAdaptiveBatch(
       selection,
       target_level,
+      topic_levels,
       {},
       session_goal);
 
@@ -172,6 +223,7 @@
       .queue = LearningQueue(question_ids),
       .selection = selection,
       .completed_question_ids = {},
+      .topic_levels = std::move(topic_levels),
       .session_goal = session_goal,
       .total_attempts = 0,
       .correct_attempts = 0,
@@ -207,6 +259,7 @@
   const auto question_ids = BuildAdaptiveBatch(
       next.selection,
       next.target_level,
+      next.topic_levels,
       next.completed_question_ids,
       remaining);
 
@@ -236,6 +289,15 @@
   return questions_.get().Get(id);
 }
 
+int QuizSessionService::TopicLevel(
+    const QuizSession& session,
+    std::string_view topic) const {
+  return TopicLevelOrFallback(
+      session.topic_levels,
+      topic,
+      session.target_level);
+}
+
 AnswerSubmission
 QuizSessionService::SubmitAnswer(
     const QuizSession& session,
@@ -270,6 +332,7 @@
 QuizSessionService::BuildAdaptiveBatch(
     const QuestionSelection& selection,
     int target_level,
+    const std::unordered_map<std::string, int>& topic_levels,
     std::span<const std::string> excluded_ids,
     std::size_t max_questions) const {
   ValidateTargetLevel(target_level);
@@ -294,15 +357,27 @@
 
   std::ranges::stable_sort(
       question_ids,
-      [this, target_level](
+      [this, target_level, &topic_levels](
           const std::string& lhs,
           const std::string& rhs) {
+        const auto& lhs_question =
+            questions_.get().Get(lhs);
+        const auto& rhs_question =
+            questions_.get().Get(rhs);
+
         const auto lhs_distance = LevelDistance(
-            questions_.get().Get(lhs).level,
-            target_level);
+            lhs_question.level,
+            TopicLevelOrFallback(
+                topic_levels,
+                lhs_question.topic,
+                target_level));
+
         const auto rhs_distance = LevelDistance(
-            questions_.get().Get(rhs).level,
-            target_level);
+            rhs_question.level,
+            TopicLevelOrFallback(
+                topic_levels,
+                rhs_question.topic,
+                target_level));
 
         return lhs_distance < rhs_distance;
       });
diff -ruN a/src/app/quiz_session_service.h b/src/app/quiz_session_service.h
--- a/src/app/quiz_session_service.h	2026-10-06 20:03:00.800534092 +0000
+++ b/src/app/quiz_session_service.h	2026-10-06 20:04:31.843921681 +0000
@@ -10,6 +10,7 @@
 #include <span>
 #include <string>
 #include <string_view>
+#include <unordered_map>
 
 struct AnswerSubmission {
   QuizSession session;
@@ -36,6 +37,12 @@
       int target_level) const;
 
   [[nodiscard]]
+  QuizSession Start(
+      const QuestionSelection& selection,
+      int target_level,
+      std::unordered_map<std::string, int> topic_levels) const;
+
+  [[nodiscard]]
   QuizSession Refill(
       const QuizSession& session) const;
 
@@ -48,6 +55,11 @@
       std::string_view id) const;
 
   [[nodiscard]]
+  int TopicLevel(
+      const QuizSession& session,
+      std::string_view topic) const;
+
+  [[nodiscard]]
   AnswerSubmission SubmitAnswer(
       const QuizSession& session,
       std::string_view answer) const;
@@ -62,6 +74,7 @@
   std::vector<std::string> BuildAdaptiveBatch(
       const QuestionSelection& selection,
       int target_level,
+      const std::unordered_map<std::string, int>& topic_levels,
       std::span<const std::string> excluded_ids,
       std::size_t max_questions) const;
 
diff -ruN a/src/bot/card_renderer.cc b/src/bot/card_renderer.cc
--- a/src/bot/card_renderer.cc	2026-10-06 20:03:00.800651899 +0000
+++ b/src/bot/card_renderer.cc	2026-10-06 20:06:54.367555679 +0000
@@ -151,6 +151,23 @@
   }
 }
 
+std::string TopicDisplayName(std::string_view topic) {
+  if (topic.empty()) {
+    throw std::invalid_argument("training topic must not be empty");
+  }
+
+  std::string result(topic);
+
+  std::ranges::replace(result, '_', ' ');
+
+  if (result.front() >= 'a' && result.front() <= 'z') {
+    result.front() = static_cast<char>(
+        result.front() - 'a' + 'A');
+  }
+
+  return result;
+}
+
 std::string BuildProgressBar(std::size_t completed, std::size_t goal) {
   if (goal == 0 || completed > goal) {
     throw std::invalid_argument("invalid training progress");
@@ -170,8 +187,15 @@
     throw std::invalid_argument("invalid training progress view");
   }
 
-  std::string text = "<b>Уровень ";
-  text += std::to_string(progress.adaptive_level);
+  if (progress.topic_level < 1 || progress.topic_level > 10 ||
+      progress.adaptive_level < 1 || progress.adaptive_level > 10) {
+    throw std::invalid_argument("invalid training level");
+  }
+
+  std::string text = "<b>";
+  text += EscapeHtml(TopicDisplayName(progress.topic));
+  text += " · ур. темы ";
+  text += std::to_string(progress.topic_level);
   text += "</b> · ";
 
   if (progress.repeat) {
@@ -187,7 +211,9 @@
     text += std::to_string(progress.session_goal);
   }
 
-  text += "\nПрогресс ";
+  text += "\nОбщий ур. ";
+  text += std::to_string(progress.adaptive_level);
+  text += " · Прогресс ";
   text += BuildProgressBar(progress.completed_questions, progress.session_goal);
   text += ' ';
   text += std::to_string(progress.completed_questions);
@@ -199,7 +225,10 @@
   if (progress.total_attempts == 0) {
     text += "—";
   } else {
-    const auto accuracy = ((progress.correct_attempts * 100) + (progress.total_attempts / 2)) / progress.total_attempts;
+    const auto accuracy =
+        ((progress.correct_attempts * 100) +
+         (progress.total_attempts / 2)) /
+        progress.total_attempts;
 
     text += std::to_string(accuracy);
     text += '%';
diff -ruN a/src/bot/card_renderer.h b/src/bot/card_renderer.h
--- a/src/bot/card_renderer.h	2026-10-06 20:03:00.800694804 +0000
+++ b/src/bot/card_renderer.h	2026-10-06 20:06:21.009579106 +0000
@@ -14,6 +14,8 @@
 #include <vector>
 
 struct TrainingProgressView {
+  std::string_view topic;
+  int topic_level = 3;
   int adaptive_level = 3;
   std::size_t completed_questions = 0;
   std::size_t session_goal = 0;
diff -ruN a/src/bot/telegram_update_handler.cc b/src/bot/telegram_update_handler.cc
--- a/src/bot/telegram_update_handler.cc	2026-10-06 20:03:00.800825890 +0000
+++ b/src/bot/telegram_update_handler.cc	2026-10-06 20:07:29.464701030 +0000
@@ -187,8 +187,12 @@
 }
 
 TrainingProgressView BuildProgressView(
-    const TelegramSession& session) {
+    const TelegramSession& session,
+    const Question& question,
+    int topic_level) {
   return {
+      .topic = question.topic,
+      .topic_level = topic_level,
       .adaptive_level =
           session.quiz.target_level,
       .completed_questions =
@@ -740,10 +744,38 @@
     const auto next_level =
         next.quiz.target_level;
 
+    const auto previous_topic_level =
+        quiz_.get().TopicLevel(
+            session.quiz,
+            question.topic);
+
+    const auto next_topic_level =
+        quiz_.get().TopicLevel(
+            next.quiz,
+            question.topic);
+
+    attempts_.get().RecordQuestionCompletion(
+        session.user_id,
+        question.id,
+        question.topic,
+        submission.result ==
+            AnswerResult::kCorrect,
+        submission.attempts,
+        next_topic_level);
+
     attempts_.get().SetAdaptiveLevel(
         session.user_id,
         next_level);
 
+    if (previous_topic_level != next_topic_level) {
+      spdlog::info(
+          "topic level changed, user_id={}, topic={}, {} -> {}",
+          session.user_id,
+          question.topic,
+          previous_topic_level,
+          next_topic_level);
+    }
+
     if (previous_level != next_level) {
       spdlog::info(
           "adaptive level changed, user_id={}, {} -> {}",
@@ -787,7 +819,12 @@
                display_answer,
                submission.result,
                correct,
-               BuildProgressView(next))},
+               BuildProgressView(
+                   next,
+                   question,
+                   quiz_.get().TopicLevel(
+                       next.quiz,
+                       question.topic)))},
           {"parse_mode", "HTML"},
           {"reply_markup",
            BuildAnswerKeyboard(
@@ -894,7 +931,12 @@
                *session.pending_explanation_answer_text,
                AnswerResult::kIncorrect,
                true,
-               BuildProgressView(session))},
+               BuildProgressView(
+                   session,
+                   question,
+                   quiz_.get().TopicLevel(
+                       session.quiz,
+                       question.topic)))},
           {"parse_mode", "HTML"},
           {"reply_markup",
            BuildAnswerKeyboard(false)},
@@ -1058,13 +1100,17 @@
   const auto target_level =
       attempts_.get().GetAdaptiveLevel(user_id);
 
+  auto topic_levels =
+      attempts_.get().LoadTopicLevels(user_id);
+
   TelegramSession session{
       .user_id = user_id,
       .quiz =
           quiz_.get().Start(
               QuestionSelectionForModes(
                   modes),
-              target_level),
+              target_level,
+              std::move(topic_levels)),
       .storage_session_id =
           attempts_.get().StartSession(
               user_id,
@@ -1130,7 +1176,12 @@
               question,
               option_order,
               session.selected_option_ids,
-              BuildProgressView(session)),
+              BuildProgressView(
+                  session,
+                  question,
+                  quiz_.get().TopicLevel(
+                      session.quiz,
+                      question.topic))),
           stop_token);
 
   session.message_id = new_message_id;
diff -ruN a/src/quiz/session.h b/src/quiz/session.h
--- a/src/quiz/session.h	2026-10-06 20:03:00.801360002 +0000
+++ b/src/quiz/session.h	2026-10-06 20:04:17.188386226 +0000
@@ -5,12 +5,14 @@
 
 #include <cstddef>
 #include <string>
+#include <unordered_map>
 #include <vector>
 
 struct QuizSession {
   LearningQueue queue;
   QuestionSelection selection;
   std::vector<std::string> completed_question_ids;
+  std::unordered_map<std::string, int> topic_levels;
   std::size_t session_goal = 0;
   std::size_t total_attempts = 0;
   std::size_t correct_attempts = 0;
diff -ruN a/src/storage/attempt_repository.cc b/src/storage/attempt_repository.cc
--- a/src/storage/attempt_repository.cc	2026-10-06 20:03:00.801494754 +0000
+++ b/src/storage/attempt_repository.cc	2026-10-06 20:06:10.904276352 +0000
@@ -10,6 +10,7 @@
 #include <string>
 #include <string_view>
 #include <system_error>
+#include <unordered_map>
 #include <utility>
 #include <vector>
 
@@ -196,6 +197,54 @@
         user_id,
         answered_at
     );
+
+CREATE TABLE IF NOT EXISTS question_progress (
+    user_id BIGINT NOT NULL
+        REFERENCES users(user_id),
+    question_id TEXT NOT NULL,
+    completed_count INTEGER NOT NULL DEFAULT 0
+        CHECK (completed_count >= 0),
+    correct_count INTEGER NOT NULL DEFAULT 0
+        CHECK (correct_count >= 0),
+    failed_count INTEGER NOT NULL DEFAULT 0
+        CHECK (failed_count >= 0),
+    first_try_correct_count INTEGER NOT NULL DEFAULT 0
+        CHECK (first_try_correct_count >= 0),
+    last_answered_at TIMESTAMPTZ NOT NULL DEFAULT now(),
+    PRIMARY KEY (user_id, question_id)
+);
+
+CREATE INDEX IF NOT EXISTS
+    idx_question_progress_user_last
+    ON question_progress(
+        user_id,
+        last_answered_at
+    );
+
+CREATE TABLE IF NOT EXISTS topic_progress (
+    user_id BIGINT NOT NULL
+        REFERENCES users(user_id),
+    topic TEXT NOT NULL,
+    adaptive_level INTEGER NOT NULL DEFAULT 3
+        CHECK (adaptive_level BETWEEN 1 AND 10),
+    completed_count INTEGER NOT NULL DEFAULT 0
+        CHECK (completed_count >= 0),
+    correct_count INTEGER NOT NULL DEFAULT 0
+        CHECK (correct_count >= 0),
+    failed_count INTEGER NOT NULL DEFAULT 0
+        CHECK (failed_count >= 0),
+    first_try_correct_count INTEGER NOT NULL DEFAULT 0
+        CHECK (first_try_correct_count >= 0),
+    last_answered_at TIMESTAMPTZ NOT NULL DEFAULT now(),
+    PRIMARY KEY (user_id, topic)
+);
+
+CREATE INDEX IF NOT EXISTS
+    idx_topic_progress_user_last
+    ON topic_progress(
+        user_id,
+        last_answered_at
+    );
 )sql");
   }
 
@@ -286,6 +335,55 @@
       false);
 }
 
+std::unordered_map<std::string, int>
+AttemptRepository::LoadTopicLevels(
+    std::int64_t user_id) {
+  EnsureUser(user_id);
+
+  const auto result = ExecuteParams(
+      impl_->connection,
+      R"sql(
+SELECT topic, adaptive_level
+FROM topic_progress
+WHERE user_id = $1;
+)sql",
+      {
+          std::to_string(user_id),
+      },
+      true);
+
+  std::unordered_map<std::string, int> levels;
+
+  const auto row_count = PQntuples(result.Get());
+
+  levels.reserve(static_cast<std::size_t>(row_count));
+
+  for (int row = 0; row < row_count; ++row) {
+    const auto* topic_value =
+        PQgetvalue(result.Get(), row, 0);
+
+    if (topic_value == nullptr || *topic_value == '\0') {
+      throw std::runtime_error(
+          "stored topic must not be empty");
+    }
+
+    const auto level =
+        ParseInt64(
+            PQgetvalue(result.Get(), row, 1));
+
+    if (level < 1 || level > 10) {
+      throw std::runtime_error(
+          "stored topic level must be between 1 and 10");
+    }
+
+    levels.emplace(
+        topic_value,
+        static_cast<int>(level));
+  }
+
+  return levels;
+}
+
 std::int64_t AttemptRepository::StartSession(std::int64_t user_id, std::string_view mode) {
   EnsureUser(user_id);
 
@@ -339,6 +437,132 @@
                 false);
 }
 
+void AttemptRepository::RecordQuestionCompletion(
+    std::int64_t user_id,
+    std::string_view question_id,
+    std::string_view topic,
+    bool correct,
+    std::size_t attempts,
+    int topic_level) {
+  if (question_id.empty()) {
+    throw std::invalid_argument(
+        "question id must not be empty");
+  }
+
+  if (topic.empty()) {
+    throw std::invalid_argument(
+        "topic must not be empty");
+  }
+
+  if (attempts < 1 || attempts > 3) {
+    throw std::invalid_argument(
+        "attempt number must be between 1 and 3");
+  }
+
+  if (topic_level < 1 || topic_level > 10) {
+    throw std::invalid_argument(
+        "topic level must be between 1 and 10");
+  }
+
+  EnsureUser(user_id);
+
+  const auto correct_increment =
+      correct ? "1" : "0";
+
+  const auto failed_increment =
+      correct ? "0" : "1";
+
+  const auto first_try_increment =
+      correct && attempts == 1 ? "1" : "0";
+
+  Execute(impl_->connection, "BEGIN");
+
+  try {
+    ExecuteParams(
+        impl_->connection,
+        R"sql(
+INSERT INTO question_progress AS progress(
+    user_id,
+    question_id,
+    completed_count,
+    correct_count,
+    failed_count,
+    first_try_correct_count,
+    last_answered_at
+)
+VALUES ($1, $2, 1, $3, $4, $5, now())
+ON CONFLICT (user_id, question_id)
+DO UPDATE SET
+    completed_count =
+        progress.completed_count + 1,
+    correct_count =
+        progress.correct_count + EXCLUDED.correct_count,
+    failed_count =
+        progress.failed_count + EXCLUDED.failed_count,
+    first_try_correct_count =
+        progress.first_try_correct_count +
+        EXCLUDED.first_try_correct_count,
+    last_answered_at = now();
+)sql",
+        {
+            std::to_string(user_id),
+            std::string(question_id),
+            correct_increment,
+            failed_increment,
+            first_try_increment,
+        },
+        false);
+
+    ExecuteParams(
+        impl_->connection,
+        R"sql(
+INSERT INTO topic_progress AS progress(
+    user_id,
+    topic,
+    adaptive_level,
+    completed_count,
+    correct_count,
+    failed_count,
+    first_try_correct_count,
+    last_answered_at
+)
+VALUES ($1, $2, $3, 1, $4, $5, $6, now())
+ON CONFLICT (user_id, topic)
+DO UPDATE SET
+    adaptive_level = EXCLUDED.adaptive_level,
+    completed_count =
+        progress.completed_count + 1,
+    correct_count =
+        progress.correct_count + EXCLUDED.correct_count,
+    failed_count =
+        progress.failed_count + EXCLUDED.failed_count,
+    first_try_correct_count =
+        progress.first_try_correct_count +
+        EXCLUDED.first_try_correct_count,
+    last_answered_at = now();
+)sql",
+        {
+            std::to_string(user_id),
+            std::string(topic),
+            std::to_string(topic_level),
+            correct_increment,
+            failed_increment,
+            first_try_increment,
+        },
+        false);
+
+    Execute(impl_->connection, "COMMIT");
+
+  } catch (...) {
+    try {
+      Execute(impl_->connection, "ROLLBACK");
+    } catch (...) {
+    }
+
+    throw;
+  }
+}
+
 void AttemptRepository::FinishSession(
     std::int64_t session_id,
     std::string_view finish_reason) {
diff -ruN a/src/storage/attempt_repository.h b/src/storage/attempt_repository.h
--- a/src/storage/attempt_repository.h	2026-10-06 20:03:00.801527613 +0000
+++ b/src/storage/attempt_repository.h	2026-10-06 20:05:41.375911732 +0000
@@ -5,6 +5,7 @@
 #include <memory>
 #include <string>
 #include <string_view>
+#include <unordered_map>
 
 class AttemptRepository {
  public:
@@ -27,6 +28,10 @@
       int level);
 
   [[nodiscard]]
+  std::unordered_map<std::string, int> LoadTopicLevels(
+      std::int64_t user_id);
+
+  [[nodiscard]]
   std::int64_t StartSession(
       std::int64_t user_id,
       std::string_view mode);
@@ -39,6 +44,14 @@
       bool correct,
       std::size_t attempt_number);
 
+  void RecordQuestionCompletion(
+      std::int64_t user_id,
+      std::string_view question_id,
+      std::string_view topic,
+      bool correct,
+      std::size_t attempts,
+      int topic_level);
+
   void FinishSession(
       std::int64_t session_id,
       std::string_view finish_reason);
PATCH

patch -p1 --dry-run <"$tmp_patch"
patch -p1 <"$tmp_patch"

echo
echo "Topic progress patch applied."
echo "Run:"
echo "  make"
echo "  make content-check"
