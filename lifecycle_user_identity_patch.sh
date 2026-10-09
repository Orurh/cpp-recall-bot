#!/usr/bin/env bash
set -euo pipefail

if [[ ! -f src/bot/telegram_update_handler.cc || ! -f src/storage/attempt_repository.cc ]]; then
  echo "Run this script from the cpp-recall-bot repository root." >&2
  exit 1
fi

patch_file="$(mktemp)"
trap 'rm -f "$patch_file"' EXIT

cat > "$patch_file" <<'PATCH'
diff --git a/src/app/application.cc b/src/app/application.cc
index 9ecd3ec..fe04bdd 100644
--- a/src/app/application.cc
+++ b/src/app/application.cc
@@ -36,26 +36,37 @@ void Application::Run(
   std::mutex wait_mutex;
   std::condition_variable_any wait_cv;
 
-  bot_.Call(
-      "setMyCommands",
-      {
-          {"commands",
-           boost::json::array{
-               boost::json::object{
-                   {"command", "start"},
-                   {"description", "Открыть главное меню"},
-               },
-               boost::json::object{
-                   {"command", "menu"},
-                   {"description", "Открыть главное меню"},
-               },
-               boost::json::object{
-                   {"command", "stop"},
-                   {"description", "Завершить тренировку"},
-               },
-           }},
-      },
-      stop_token);
+  try {
+    bot_.Call(
+        "setMyCommands",
+        {
+            {"commands",
+             boost::json::array{
+                 boost::json::object{
+                     {"command", "start"},
+                     {"description", "Начать заново"},
+                 },
+                 boost::json::object{
+                     {"command", "menu"},
+                     {"description", "Открыть главное меню"},
+                 },
+                 boost::json::object{
+                     {"command", "stop"},
+                     {"description", "Остановить тренировку"},
+                 },
+             }},
+        },
+        stop_token);
+
+  } catch (const std::exception& error) {
+    if (stop_token.stop_requested()) {
+      return;
+    }
+
+    spdlog::warn(
+        "failed to configure Telegram commands: {}",
+        error.what());
+  }
 
   spdlog::info(
       "application started, questions={}",
diff --git a/src/bot/card_renderer.cc b/src/bot/card_renderer.cc
index 0bc5c4e..8a40bb1 100644
--- a/src/bot/card_renderer.cc
+++ b/src/bot/card_renderer.cc
@@ -184,12 +184,24 @@ boost::json::object BuildStartMenuKeyboard(std::span<const TrainingContentMode>
   };
 }
 
-boost::json::object BuildStartMenuMessage(std::int64_t chat_id, std::span<const TrainingContentMode> selected_modes) {
+boost::json::object BuildStartMenuMessage(
+    std::int64_t chat_id,
+    std::span<const TrainingContentMode> selected_modes,
+    std::string_view notice) {
+  std::string text;
+
+  if (!notice.empty()) {
+    text += notice;
+    text += "\n\n";
+  }
+
+  text +=
+      "Что тренируем?\n\n"
+      "Выбери один или несколько режимов.";
+
   return {
       {"chat_id", chat_id},
-      {"text",
-       "Что тренируем?\n\n"
-       "Выбери один или несколько режимов."},
+      {"text", std::move(text)},
       {"reply_markup", BuildStartMenuKeyboard(selected_modes)},
   };
 }
@@ -198,8 +210,8 @@ boost::json::object BuildQuestionKeyboard(const Question& question, const std::v
                                           const std::vector<std::string>& selected_option_ids) {
   boost::json::array keyboard;
 
-  if (const auto* task = std::get_if<SingleChoiceTask>(&question.task)) {
-    ValidateOptionOrder(task->options, option_order);
+  if (const auto* single_task = std::get_if<SingleChoiceTask>(&question.task)) {
+    ValidateOptionOrder(single_task->options, option_order);
 
     for (std::size_t i = 0; i < option_order.size(); ++i) {
       const auto& option_id = option_order[i];
@@ -294,16 +306,6 @@ boost::json::object BuildAnswerKeyboard(bool show_explanation_button) {
   };
 }
 
-boost::json::object BuildMainMenuKeyboard() {
-  boost::json::array keyboard;
-
-  AddButton(keyboard, "Главное меню", "menu");
-
-  return {
-      {"inline_keyboard", std::move(keyboard)},
-  };
-}
-
 std::string BuildAnswerDisplayText(const Question& question, const std::vector<std::string>& option_order,
                                    std::string_view answer) {
   if (std::holds_alternative<SingleChoiceTask>(question.task)) {
diff --git a/src/bot/card_renderer.h b/src/bot/card_renderer.h
index add1677..c9d1490 100644
--- a/src/bot/card_renderer.h
+++ b/src/bot/card_renderer.h
@@ -14,7 +14,8 @@
 
 boost::json::object BuildStartMenuMessage(
     std::int64_t chat_id,
-    std::span<const TrainingContentMode> selected_modes);
+    std::span<const TrainingContentMode> selected_modes,
+    std::string_view notice = {});
 
 boost::json::object BuildStartMenuKeyboard(
     std::span<const TrainingContentMode> selected_modes);
@@ -33,7 +34,6 @@ boost::json::object BuildQuestionKeyboard(
 boost::json::object BuildAnswerKeyboard(
     bool show_explanation_button);
 
-boost::json::object BuildMainMenuKeyboard();
 
 std::string BuildAnswerDisplayText(
     const Question& question,
diff --git a/src/bot/telegram_session.h b/src/bot/telegram_session.h
index bb0cff2..ec04fb3 100644
--- a/src/bot/telegram_session.h
+++ b/src/bot/telegram_session.h
@@ -8,6 +8,7 @@
 #include <vector>
 
 struct TelegramSession {
+  std::int64_t user_id = 0;
   QuizSession quiz;
   std::int64_t storage_session_id = 0;
   std::optional<std::int64_t> message_id;
diff --git a/src/bot/telegram_update_handler.cc b/src/bot/telegram_update_handler.cc
index 66ba774..d75d663 100644
--- a/src/bot/telegram_update_handler.cc
+++ b/src/bot/telegram_update_handler.cc
@@ -22,6 +22,40 @@
 
 namespace {
 
+enum class BotCommand : std::uint8_t {
+  kStart,
+  kMenu,
+  kStop,
+  kUnknown,
+};
+
+std::optional<BotCommand> ParseBotCommand(std::string_view input) {
+  if (input.empty() || input.front() != '/') {
+    return std::nullopt;
+  }
+
+  const auto end = input.find_first_of(" \t\r\n");
+  auto token = input.substr(0, end);
+
+  if (const auto at = token.find('@'); at != std::string_view::npos) {
+    token = token.substr(0, at);
+  }
+
+  if (token == "/start") {
+    return BotCommand::kStart;
+  }
+
+  if (token == "/menu") {
+    return BotCommand::kMenu;
+  }
+
+  if (token == "/stop") {
+    return BotCommand::kStop;
+  }
+
+  return BotCommand::kUnknown;
+}
+
 void DeleteMessageBestEffort(
     TelegramBot& bot,
     std::int64_t chat_id,
@@ -206,6 +240,12 @@ void TelegramUpdateHandler::HandleMessage(
   const auto chat_id =
       chat.at("id").as_int64();
 
+  const auto& from =
+      message.at("from").as_object();
+
+  const auto user_id =
+      from.at("id").as_int64();
+
   const auto& text =
       text_value->as_string();
 
@@ -213,24 +253,45 @@ void TelegramUpdateHandler::HandleMessage(
       text.data(),
       text.size());
 
-  if (input == "/start" ||
-      input == "/menu" ||
-      input == "/stop") {
-    ShowStartMenu(
-        chat_id,
-        std::nullopt,
-        stop_token);
-    return;
-  }
+  if (const auto command = ParseBotCommand(input)) {
+    switch (*command) {
+      case BotCommand::kStart:
+        ShowStartMenu(
+            user_id,
+            chat_id,
+            std::nullopt,
+            MenuAction::kStart,
+            stop_token);
+        return;
 
-  if (input.starts_with('/')) {
-    return;
+      case BotCommand::kMenu:
+        ShowStartMenu(
+            user_id,
+            chat_id,
+            std::nullopt,
+            MenuAction::kMenu,
+            stop_token);
+        return;
+
+      case BotCommand::kStop:
+        ShowStartMenu(
+            user_id,
+            chat_id,
+            std::nullopt,
+            MenuAction::kStop,
+            stop_token);
+        return;
+
+      case BotCommand::kUnknown:
+        return;
+    }
   }
 
   const auto session_it =
       sessions_.find(chat_id);
 
-  if (session_it == sessions_.end()) {
+  if (session_it == sessions_.end() ||
+      session_it->second.user_id != user_id) {
     return;
   }
 
@@ -341,6 +402,12 @@ void TelegramUpdateHandler::HandleCallback(
   const auto chat_id =
       chat.at("id").as_int64();
 
+  const auto& from =
+      callback.at("from").as_object();
+
+  const auto user_id =
+      from.at("id").as_int64();
+
   const auto message_id =
       message.at("message_id").as_int64();
 
@@ -363,7 +430,8 @@ void TelegramUpdateHandler::HandleCallback(
     if (const auto session_it =
             sessions_.find(chat_id);
         session_it != sessions_.end()) {
-      if (!session_it->second.message_id ||
+      if (session_it->second.user_id != user_id ||
+          !session_it->second.message_id ||
           *session_it->second.message_id !=
               message_id) {
         return;
@@ -373,14 +441,16 @@ void TelegramUpdateHandler::HandleCallback(
     if (const auto menu_it =
             start_menus_.find(chat_id);
         menu_it != start_menus_.end() &&
-        menu_it->second.message_id !=
-            message_id) {
+        (menu_it->second.user_id != user_id ||
+         menu_it->second.message_id != message_id)) {
       return;
     }
 
     ShowStartMenu(
+        user_id,
         chat_id,
         message_id,
+        MenuAction::kMenu,
         stop_token);
     return;
   }
@@ -388,6 +458,7 @@ void TelegramUpdateHandler::HandleCallback(
   if (const auto menu_it =
           start_menus_.find(chat_id);
       menu_it != start_menus_.end() &&
+      menu_it->second.user_id == user_id &&
       menu_it->second.message_id ==
           message_id) {
     HandleStartMenuCallback(
@@ -409,7 +480,8 @@ void TelegramUpdateHandler::HandleCallback(
   auto& session =
       session_it->second;
 
-  if (!session.message_id ||
+  if (session.user_id != user_id ||
+      !session.message_id ||
       *session.message_id != message_id) {
     return;
   }
@@ -571,7 +643,7 @@ bool TelegramUpdateHandler::ApplySubmission(
   }
 
   attempts_.get().RecordAttempt(
-      chat_id,
+      session.user_id,
       session.storage_session_id,
       question.id,
       storage_answer,
@@ -735,10 +807,35 @@ void TelegramUpdateHandler::ShowExplanation(
 }
 
 void TelegramUpdateHandler::ShowStartMenu(
+    std::int64_t user_id,
     std::int64_t chat_id,
     std::optional<std::int64_t> source_message_id,
+    MenuAction action,
     const std::stop_token& stop_token) {
-  attempts_.get().EnsureUser(chat_id);
+  attempts_.get().EnsureUser(user_id);
+
+  std::string_view finish_reason;
+  std::string_view notice;
+
+  switch (action) {
+    case MenuAction::kStart:
+      finish_reason = "restarted";
+      break;
+
+    case MenuAction::kMenu:
+      finish_reason = "menu";
+      break;
+
+    case MenuAction::kStop:
+      finish_reason = "stopped";
+      notice = "Тренировка остановлена.";
+      break;
+
+    case MenuAction::kCompleted:
+      finish_reason = "completed";
+      notice = "Тренировка завершена.";
+      break;
+  }
 
   std::optional<std::int64_t>
       previous_message_id = source_message_id;
@@ -746,18 +843,37 @@ void TelegramUpdateHandler::ShowStartMenu(
   if (const auto session_it =
           sessions_.find(chat_id);
       session_it != sessions_.end()) {
+    if (session_it->second.user_id != user_id) {
+      return;
+    }
+
     previous_message_id =
         session_it->second.message_id;
 
     FinishStorageSession(
-        session_it->second);
+        session_it->second,
+        finish_reason);
 
     sessions_.erase(session_it);
+
+  } else if (action != MenuAction::kCompleted) {
+    const auto orphan_reason =
+        action == MenuAction::kStop
+            ? std::string_view{"stopped"}
+            : std::string_view{"interrupted"};
+
+    attempts_.get().FinishOpenSessions(
+        user_id,
+        orphan_reason);
   }
 
   if (const auto menu_it =
           start_menus_.find(chat_id);
       menu_it != start_menus_.end()) {
+    if (menu_it->second.user_id != user_id) {
+      return;
+    }
+
     previous_message_id =
         menu_it->second.message_id;
   }
@@ -771,12 +887,14 @@ void TelegramUpdateHandler::ShowStartMenu(
           previous_message_id,
           BuildStartMenuMessage(
               chat_id,
-              selected_modes),
+              selected_modes,
+              notice),
           stop_token);
 
   start_menus_.insert_or_assign(
       chat_id,
       StartMenuState{
+          .user_id = user_id,
           .message_id =
               new_message_id,
           .selected_modes = {},
@@ -827,17 +945,21 @@ void TelegramUpdateHandler::StartTraining(
     return;
   }
 
+  const auto user_id =
+      menu_it->second.user_id;
+
   const auto storage_mode =
       TrainingModeStorageName(modes);
 
   TelegramSession session{
+      .user_id = user_id,
       .quiz =
           quiz_.get().Start(
               QuestionSelectionForModes(
                   modes)),
       .storage_session_id =
           attempts_.get().StartSession(
-              chat_id,
+              user_id,
               storage_mode),
       .message_id =
           menu_it->second.message_id,
@@ -856,7 +978,9 @@ void TelegramUpdateHandler::StartTraining(
         stop_token);
 
   } catch (...) {
-    FinishStorageSession(session);
+    FinishStorageSession(
+        session,
+        "start_failed");
     throw;
   }
 
@@ -915,27 +1039,12 @@ void TelegramUpdateHandler::NextQuestion(
     return;
   }
 
-  FinishStorageSession(session);
-
-  boost::json::object message{
-      {"chat_id", chat_id},
-      {"text",
-       "На сегодня тренировка завершена."},
-      {"reply_markup",
-       BuildMainMenuKeyboard()},
-  };
-
-  session.message_id =
-      SendReplacingMessage(
-          chat_id,
-          session.message_id,
-          std::move(message),
-          stop_token);
-
-  session.option_order.clear();
-  session.selected_option_ids.clear();
-  session.pending_explanation_question_id.reset();
-  session.pending_explanation_answer_text.reset();
+  ShowStartMenu(
+      session.user_id,
+      chat_id,
+      session.message_id,
+      MenuAction::kCompleted,
+      stop_token);
 }
 
 std::int64_t
@@ -970,13 +1079,15 @@ TelegramUpdateHandler::SendReplacingMessage(
 }
 
 void TelegramUpdateHandler::FinishStorageSession(
-    TelegramSession& session) {
+    TelegramSession& session,
+    std::string_view finish_reason) {
   if (session.storage_session_id == 0) {
     return;
   }
 
   attempts_.get().FinishSession(
-      session.storage_session_id);
+      session.storage_session_id,
+      finish_reason);
 
   session.storage_session_id = 0;
 }
@@ -988,7 +1099,9 @@ void TelegramUpdateHandler::FinishActiveSessions() {
     }
 
     try {
-      FinishStorageSession(session);
+      FinishStorageSession(
+          session,
+          "shutdown");
 
     } catch (const std::exception& error) {
       spdlog::warn(
diff --git a/src/bot/telegram_update_handler.h b/src/bot/telegram_update_handler.h
index 8109923..1b9fcd4 100644
--- a/src/bot/telegram_update_handler.h
+++ b/src/bot/telegram_update_handler.h
@@ -32,7 +32,15 @@ class TelegramUpdateHandler {
   void FinishActiveSessions();
 
  private:
+  enum class MenuAction : std::uint8_t {
+    kStart,
+    kMenu,
+    kStop,
+    kCompleted,
+  };
+
   struct StartMenuState {
+    std::int64_t user_id = 0;
     std::int64_t message_id = 0;
     std::vector<TrainingContentMode> selected_modes;
   };
@@ -96,8 +104,10 @@ class TelegramUpdateHandler {
       const std::stop_token& stop_token);
 
   void ShowStartMenu(
+      std::int64_t user_id,
       std::int64_t chat_id,
       std::optional<std::int64_t> source_message_id,
+      MenuAction action,
       const std::stop_token& stop_token);
 
   void ToggleTrainingMode(
@@ -129,7 +139,8 @@ class TelegramUpdateHandler {
       const std::stop_token& stop_token);
 
   void FinishStorageSession(
-      TelegramSession& session);
+      TelegramSession& session,
+      std::string_view finish_reason);
 
   std::reference_wrapper<TelegramBot> bot_;
   std::reference_wrapper<QuizSessionService> quiz_;
diff --git a/src/storage/attempt_repository.cc b/src/storage/attempt_repository.cc
index d3f95fa..e04e0d3 100644
--- a/src/storage/attempt_repository.cc
+++ b/src/storage/attempt_repository.cc
@@ -153,9 +153,13 @@ CREATE TABLE IF NOT EXISTS sessions (
         REFERENCES users(user_id),
     started_at TIMESTAMPTZ NOT NULL DEFAULT now(),
     finished_at TIMESTAMPTZ,
-    mode TEXT NOT NULL
+    mode TEXT NOT NULL,
+    finish_reason TEXT
 );
 
+ALTER TABLE sessions
+    ADD COLUMN IF NOT EXISTS finish_reason TEXT;
+
 CREATE INDEX IF NOT EXISTS
     idx_sessions_user_started
     ON sessions(user_id, started_at);
@@ -275,16 +279,46 @@ VALUES ($1, $2, $3, $4, $5, $6);
                 false);
 }
 
-void AttemptRepository::FinishSession(std::int64_t session_id) {
+void AttemptRepository::FinishSession(
+    std::int64_t session_id,
+    std::string_view finish_reason) {
+  if (finish_reason.empty()) {
+    throw std::invalid_argument("finish reason must not be empty");
+  }
+
   ExecuteParams(impl_->connection,
                 R"sql(
 UPDATE sessions
-SET finished_at = now()
+SET finished_at = now(),
+    finish_reason = $2
 WHERE id = $1
   AND finished_at IS NULL;
 )sql",
                 {
                     std::to_string(session_id),
+                    std::string(finish_reason),
+                },
+                false);
+}
+
+void AttemptRepository::FinishOpenSessions(
+    std::int64_t user_id,
+    std::string_view finish_reason) {
+  if (finish_reason.empty()) {
+    throw std::invalid_argument("finish reason must not be empty");
+  }
+
+  ExecuteParams(impl_->connection,
+                R"sql(
+UPDATE sessions
+SET finished_at = now(),
+    finish_reason = $2
+WHERE user_id = $1
+  AND finished_at IS NULL;
+)sql",
+                {
+                    std::to_string(user_id),
+                    std::string(finish_reason),
                 },
                 false);
 }
diff --git a/src/storage/attempt_repository.h b/src/storage/attempt_repository.h
index d603bd3..a0d3ff3 100644
--- a/src/storage/attempt_repository.h
+++ b/src/storage/attempt_repository.h
@@ -32,7 +32,13 @@ class AttemptRepository {
       bool correct,
       std::size_t attempt_number);
 
-  void FinishSession(std::int64_t session_id);
+  void FinishSession(
+      std::int64_t session_id,
+      std::string_view finish_reason);
+
+  void FinishOpenSessions(
+      std::int64_t user_id,
+      std::string_view finish_reason);
 
  private:
   struct Impl;
PATCH

git apply --check "$patch_file"
git apply "$patch_file"

echo "Patch applied."
echo "Run: make && make content-check"
