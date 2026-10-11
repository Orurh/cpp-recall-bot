#pragma once

#include "app/quiz_session_service.h"
#include "app/training_mode.h"
#include "bot/telegram_bot.h"
#include "bot/telegram_session.h"
#include "quiz/matching_round.h"
#include "quiz/question.h"
#include "storage/attempt_repository.h"

#include <boost/json/object.hpp>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

class TelegramUpdateHandler {
 public:
  TelegramUpdateHandler(TelegramBot& bot, QuizSessionService& quiz, AttemptRepository& attempts);

  void Handle(const boost::json::object& update, const std::stop_token& stop_token);

  void FinishActiveSessions();

 private:
  enum class MenuAction : std::uint8_t {
    kStart,
    kMenu,
    kStop,
    kCompleted,
  };

  struct StartMenuState {
    std::int64_t user_id = 0;
    std::int64_t message_id = 0;
    std::vector<TrainingContentMode> selected_modes;
  };

  void HandleMessage(const boost::json::object& message, const std::stop_token& stop_token);

  void HandleCallback(const boost::json::object& callback, const std::stop_token& stop_token);

  void HandleTextAnswer(std::int64_t chat_id, const boost::json::object& message, std::string_view answer,
                        TelegramSession& session, const std::stop_token& stop_token);

  void HandleStartMenuCallback(std::int64_t chat_id, std::int64_t message_id, std::string_view payload,
                               StartMenuState& menu, const std::stop_token& stop_token);

  void HandleSessionCallback(std::int64_t chat_id, std::int64_t message_id, std::string_view payload,
                             TelegramSession& session, const std::stop_token& stop_token);

  bool ApplySubmission(std::int64_t chat_id, TelegramSession& session, const Question& question,
                       const std::string& storage_answer, const std::string& display_answer,
                       AnswerSubmission submission, const std::stop_token& stop_token);

  void ToggleOption(std::int64_t chat_id, std::int64_t message_id, const Question& question, std::string_view option_id,
                    TelegramSession& session, const std::stop_token& stop_token);

  void SubmitMultipleChoice(std::int64_t chat_id, const Question& question, TelegramSession& session,
                            const std::stop_token& stop_token);

  void HandleMatchingPick(
      std::int64_t chat_id,
      const Question& question,
      MatchingSide side,
      std::size_t position,
      std::size_t revision,
      TelegramSession& session,
      const std::stop_token& stop_token);

  void ShowExplanation(
      std::int64_t chat_id,
      std::int64_t message_id,
      TelegramSession& session,
      const std::stop_token& stop_token);

  void ShowStartMenu(std::int64_t user_id, std::int64_t chat_id, std::optional<std::int64_t> source_message_id,
                     MenuAction action, const std::stop_token& stop_token);

  void ShowProgressSummary(std::int64_t user_id, std::int64_t chat_id, const std::stop_token& stop_token);

  void ToggleTrainingMode(std::int64_t chat_id, std::int64_t message_id, TrainingContentMode mode, StartMenuState& menu,
                          const std::stop_token& stop_token);

  void StartTraining(std::int64_t chat_id, const std::vector<TrainingContentMode>& modes,
                     const std::stop_token& stop_token);

  void SendQuestion(std::int64_t chat_id, TelegramSession& session, const std::stop_token& stop_token);

  void NextQuestion(std::int64_t chat_id, TelegramSession& session, const std::stop_token& stop_token);

  std::int64_t SendReplacingMessage(std::int64_t chat_id, std::optional<std::int64_t> previous_message_id,
                                    boost::json::object message, const std::stop_token& stop_token);

  void FinishStorageSession(TelegramSession& session, std::string_view finish_reason);

  std::reference_wrapper<TelegramBot> bot_;
  std::reference_wrapper<QuizSessionService> quiz_;
  std::reference_wrapper<AttemptRepository> attempts_;

  std::unordered_map<std::int64_t, TelegramSession> sessions_;

  std::unordered_map<std::int64_t, StartMenuState> start_menus_;
};
