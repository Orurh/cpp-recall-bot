#!/usr/bin/env bash
set -euo pipefail

if [[ ! -f src/app/quiz_session_service.cc || ! -f src/bot/telegram_update_handler.cc ]]; then
  echo "run this script from the cpp-recall-bot repository root" >&2
  exit 1
fi

patch -p1 <<'PATCH'
diff -ruN adaptive_orig/src/app/quiz_session_service.cc adaptive_verify/src/app/quiz_session_service.cc
--- adaptive_orig/src/app/quiz_session_service.cc	2026-10-05 23:22:48.031117323 +0000
+++ adaptive_verify/src/app/quiz_session_service.cc	2026-10-05 23:21:25.604695563 +0000
@@ -7,34 +7,88 @@
 #include "quiz/session.h"
 
 #include <algorithm>
+#include <cstddef>
 #include <random>
 #include <span>
 #include <stdexcept>
 #include <string>
 #include <string_view>
 #include <utility>
+#include <vector>
 
 namespace {
 
+constexpr int kMinLevel = 1;
+constexpr int kMaxLevel = 10;
+constexpr int kInitialAdaptiveLevel = 3;
+constexpr std::size_t kAdaptiveBatchSize = 3;
+
+void ValidateTargetLevel(int level) {
+  if (level < kMinLevel || level > kMaxLevel) {
+    throw std::invalid_argument(
+        "adaptive target level must be between 1 and 10");
+  }
+}
+
+int LevelDistance(int lhs, int rhs) {
+  return lhs >= rhs ? lhs - rhs : rhs - lhs;
+}
+
+int NextTargetLevel(
+    const Question& question,
+    AnswerResult result,
+    std::size_t attempts) {
+  if (result == AnswerResult::kCorrect && attempts == 1) {
+    return std::min(question.level + 1, kMaxLevel);
+  }
+
+  if (result == AnswerResult::kCorrect && attempts == 2) {
+    return question.level;
+  }
+
+  return std::max(question.level - 1, kMinLevel);
+}
+
 AnswerSubmission BuildSubmission(
     const QuizSession& session,
+    const Question& question,
     AnswerResult result) {
+  if (result == AnswerResult::kUnknownOption) {
+    return {
+        .session = session,
+        .result = result,
+        .attempts = 0,
+        .completed = false,
+    };
+  }
+
   QuizSession next = session;
 
   const auto submit_result =
       next.queue.SubmitAnswer(
-          result ==
-          AnswerResult::kCorrect);
+          result == AnswerResult::kCorrect);
 
   next.answered = true;
 
+  if (submit_result.completed) {
+    if (std::ranges::find(
+            next.completed_question_ids,
+            question.id) ==
+        next.completed_question_ids.end()) {
+      next.completed_question_ids.push_back(question.id);
+    }
+
+    next.target_level = NextTargetLevel(
+        question,
+        result,
+        submit_result.attempts);
+  }
+
   return {
       .session = std::move(next),
       .result = result,
-      .attempts =
-          submit_result.attempts,
-      .completed =
-          submit_result.completed,
+      .attempts = submit_result.attempts,
+      .completed = submit_result.completed,
   };
 }
 
@@ -45,33 +99,62 @@
     : questions_(questions) {}
 
 QuizSession QuizSessionService::Start() const {
-  return Start(QuestionSelection{});
+  return Start(
+      QuestionSelection{},
+      kInitialAdaptiveLevel);
 }
 
 QuizSession QuizSessionService::Start(
     const QuestionSelection& selection) const {
-  auto question_ids =
-      questions_.get().Select(selection);
+  return Start(
+      selection,
+      kInitialAdaptiveLevel);
+}
+
+QuizSession QuizSessionService::Start(
+    const QuestionSelection& selection,
+    int target_level) const {
+  ValidateTargetLevel(target_level);
+
+  auto question_ids = BuildAdaptiveBatch(
+      selection,
+      target_level,
+      {});
 
   if (question_ids.empty()) {
     throw std::runtime_error(
         "no questions match selection");
   }
 
-  std::mt19937 generator{
-      std::random_device{}()};
-
-  std::ranges::shuffle(
-      question_ids,
-      generator);
-
   return {
-      .queue =
-          LearningQueue(question_ids),
+      .queue = LearningQueue(question_ids),
+      .selection = selection,
+      .completed_question_ids = {},
+      .target_level = target_level,
       .answered = false,
   };
 }
 
+QuizSession QuizSessionService::Refill(
+    const QuizSession& session) const {
+  if (!session.queue.Empty()) {
+    throw std::logic_error(
+        "cannot refill non-empty quiz queue");
+  }
+
+  QuizSession next = session;
+
+  const auto question_ids = BuildAdaptiveBatch(
+      next.selection,
+      next.target_level,
+      next.completed_question_ids);
+
+  next.queue = LearningQueue(question_ids);
+  next.answered = false;
+
+  return next;
+}
+
 const Question&
 QuizSessionService::CurrentQuestion(
     const QuizSession& session) const {
@@ -101,6 +184,7 @@
 
   return BuildSubmission(
       session,
+      question,
       CheckAnswer(
           question,
           answer));
@@ -115,7 +199,55 @@
 
   return BuildSubmission(
       session,
+      question,
       CheckAnswer(
           question,
           answers));
 }
+
+std::vector<std::string>
+QuizSessionService::BuildAdaptiveBatch(
+    const QuestionSelection& selection,
+    int target_level,
+    std::span<const std::string> excluded_ids) const {
+  ValidateTargetLevel(target_level);
+
+  auto question_ids =
+      questions_.get().Select(selection);
+
+  std::erase_if(
+      question_ids,
+      [excluded_ids](const std::string& id) {
+        return std::ranges::find(
+                   excluded_ids,
+                   id) != excluded_ids.end();
+      });
+
+  static thread_local std::mt19937 generator{
+      std::random_device{}()};
+
+  std::ranges::shuffle(
+      question_ids,
+      generator);
+
+  std::ranges::stable_sort(
+      question_ids,
+      [this, target_level](
+          const std::string& lhs,
+          const std::string& rhs) {
+        const auto lhs_distance = LevelDistance(
+            questions_.get().Get(lhs).level,
+            target_level);
+        const auto rhs_distance = LevelDistance(
+            questions_.get().Get(rhs).level,
+            target_level);
+
+        return lhs_distance < rhs_distance;
+      });
+
+  if (question_ids.size() > kAdaptiveBatchSize) {
+    question_ids.resize(kAdaptiveBatchSize);
+  }
+
+  return question_ids;
+}
diff -ruN adaptive_orig/src/app/quiz_session_service.h adaptive_verify/src/app/quiz_session_service.h
--- adaptive_orig/src/app/quiz_session_service.h	2026-10-05 23:22:48.031150993 +0000
+++ adaptive_verify/src/app/quiz_session_service.h	2026-10-05 23:21:25.603987583 +0000
@@ -31,6 +31,15 @@
       const QuestionSelection& selection) const;
 
   [[nodiscard]]
+  QuizSession Start(
+      const QuestionSelection& selection,
+      int target_level) const;
+
+  [[nodiscard]]
+  QuizSession Refill(
+      const QuizSession& session) const;
+
+  [[nodiscard]]
   const Question& CurrentQuestion(
       const QuizSession& session) const;
 
@@ -49,7 +58,11 @@
       std::span<const std::string> answers) const;
 
  private:
-  std::reference_wrapper<
-      const QuestionBank>
-      questions_;
+  [[nodiscard]]
+  std::vector<std::string> BuildAdaptiveBatch(
+      const QuestionSelection& selection,
+      int target_level,
+      std::span<const std::string> excluded_ids) const;
+
+  std::reference_wrapper<const QuestionBank> questions_;
 };
diff -ruN adaptive_orig/src/bot/card_renderer.cc adaptive_verify/src/bot/card_renderer.cc
--- adaptive_orig/src/bot/card_renderer.cc	2026-10-05 23:22:48.031242438 +0000
+++ adaptive_verify/src/bot/card_renderer.cc	2026-10-05 23:21:50.702898220 +0000
@@ -197,7 +197,8 @@
 
   text +=
       "Что тренируем?\n\n"
-      "Выбери один или несколько режимов.";
+      "Выбери один или несколько режимов.\n"
+      "Сложность подбирается автоматически.";
 
   return {
       {"chat_id", chat_id},
diff -ruN adaptive_orig/src/bot/telegram_update_handler.cc adaptive_verify/src/bot/telegram_update_handler.cc
--- adaptive_orig/src/bot/telegram_update_handler.cc	2026-10-05 23:22:48.031436914 +0000
+++ adaptive_verify/src/bot/telegram_update_handler.cc	2026-10-05 23:21:50.703105575 +0000
@@ -656,6 +656,25 @@
   next.quiz =
       std::move(submission.session);
 
+  if (submission.completed) {
+    const auto previous_level =
+        session.quiz.target_level;
+    const auto next_level =
+        next.quiz.target_level;
+
+    attempts_.get().SetAdaptiveLevel(
+        session.user_id,
+        next_level);
+
+    if (previous_level != next_level) {
+      spdlog::info(
+          "adaptive level changed, user_id={}, {} -> {}",
+          session.user_id,
+          previous_level,
+          next_level);
+    }
+  }
+
   next.selected_option_ids.clear();
 
   const bool correct =
@@ -951,12 +970,16 @@
   const auto storage_mode =
       TrainingModeStorageName(modes);
 
+  const auto target_level =
+      attempts_.get().GetAdaptiveLevel(user_id);
+
   TelegramSession session{
       .user_id = user_id,
       .quiz =
           quiz_.get().Start(
               QuestionSelectionForModes(
-                  modes)),
+                  modes),
+              target_level),
       .storage_session_id =
           attempts_.get().StartSession(
               user_id,
@@ -1033,6 +1056,17 @@
 
   if (!session.quiz.queue.Empty()) {
     SendQuestion(
+        chat_id,
+        session,
+        stop_token);
+    return;
+  }
+
+  session.quiz =
+      quiz_.get().Refill(session.quiz);
+
+  if (!session.quiz.queue.Empty()) {
+    SendQuestion(
         chat_id,
         session,
         stop_token);
diff -ruN adaptive_orig/src/quiz/session.h adaptive_verify/src/quiz/session.h
--- adaptive_orig/src/quiz/session.h	2026-10-05 23:22:48.032092821 +0000
+++ adaptive_verify/src/quiz/session.h	2026-10-05 23:21:25.603236371 +0000
@@ -1,8 +1,15 @@
 #pragma once
 
 #include "learning/learning_queue.h"
+#include "quiz/question_bank.h"
+
+#include <string>
+#include <vector>
 
 struct QuizSession {
   LearningQueue queue;
+  QuestionSelection selection;
+  std::vector<std::string> completed_question_ids;
+  int target_level = 3;
   bool answered = false;
 };
diff -ruN adaptive_orig/src/storage/attempt_repository.cc adaptive_verify/src/storage/attempt_repository.cc
--- adaptive_orig/src/storage/attempt_repository.cc	2026-10-05 23:22:48.032187461 +0000
+++ adaptive_verify/src/storage/attempt_repository.cc	2026-10-05 23:21:37.929375487 +0000
@@ -144,9 +144,13 @@
 CREATE TABLE IF NOT EXISTS users (
     user_id BIGINT PRIMARY KEY,
     created_at TIMESTAMPTZ NOT NULL DEFAULT now(),
-    target_date DATE
+    target_date DATE,
+    adaptive_level INTEGER NOT NULL DEFAULT 3
 );
 
+ALTER TABLE users
+    ADD COLUMN IF NOT EXISTS adaptive_level INTEGER NOT NULL DEFAULT 3;
+
 CREATE TABLE IF NOT EXISTS sessions (
     id BIGSERIAL PRIMARY KEY,
     user_id BIGINT NOT NULL
@@ -226,6 +230,62 @@
                 false);
 }
 
+int AttemptRepository::GetAdaptiveLevel(std::int64_t user_id) {
+  EnsureUser(user_id);
+
+  const auto result = ExecuteParams(
+      impl_->connection,
+      R"sql(
+SELECT adaptive_level
+FROM users
+WHERE user_id = $1;
+)sql",
+      {
+          std::to_string(user_id),
+      },
+      true);
+
+  if (PQntuples(result.Get()) != 1 ||
+      PQnfields(result.Get()) != 1) {
+    throw std::runtime_error(
+        "unexpected PostgreSQL result when reading adaptive level");
+  }
+
+  const auto level = ParseInt64(
+      PQgetvalue(result.Get(), 0, 0));
+
+  if (level < 1 || level > 10) {
+    throw std::runtime_error(
+        "stored adaptive level must be between 1 and 10");
+  }
+
+  return static_cast<int>(level);
+}
+
+void AttemptRepository::SetAdaptiveLevel(
+    std::int64_t user_id,
+    int level) {
+  if (level < 1 || level > 10) {
+    throw std::invalid_argument(
+        "adaptive level must be between 1 and 10");
+  }
+
+  EnsureUser(user_id);
+
+  ExecuteParams(
+      impl_->connection,
+      R"sql(
+UPDATE users
+SET adaptive_level = $2
+WHERE user_id = $1;
+)sql",
+      {
+          std::to_string(user_id),
+          std::to_string(level),
+      },
+      false);
+}
+
 std::int64_t AttemptRepository::StartSession(std::int64_t user_id, std::string_view mode) {
   EnsureUser(user_id);
 
diff -ruN adaptive_orig/src/storage/attempt_repository.h adaptive_verify/src/storage/attempt_repository.h
--- adaptive_orig/src/storage/attempt_repository.h	2026-10-05 23:22:48.032217986 +0000
+++ adaptive_verify/src/storage/attempt_repository.h	2026-10-05 23:21:37.929222121 +0000
@@ -20,6 +20,13 @@
   void EnsureUser(std::int64_t user_id);
 
   [[nodiscard]]
+  int GetAdaptiveLevel(std::int64_t user_id);
+
+  void SetAdaptiveLevel(
+      std::int64_t user_id,
+      int level);
+
+  [[nodiscard]]
   std::int64_t StartSession(
       std::int64_t user_id,
       std::string_view mode);
PATCH

echo "adaptive level patch applied"
