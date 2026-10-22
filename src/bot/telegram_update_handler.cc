#include "telegram_update_handler.h"

#include "app/training_mode.h"
#include "bot/card_renderer.h"
#include "quiz/topic_catalog.h"

#include <spdlog/spdlog.h>
#include <boost/json.hpp>

#include <algorithm>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <optional>
#include <random>
#include <span>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <variant>
#include <vector>

namespace {

enum class BotCommand : std::uint8_t {
  kStart,
  kMenu,
  kStop,
  kStats,
  kUnknown,
};

std::optional<BotCommand> ParseBotCommand(std::string_view input) {
  if (input.empty() || input.front() != '/') {
    return std::nullopt;
  }

  const auto end = input.find_first_of(" \t\r\n");
  auto token = input.substr(0, end);

  if (const auto at = token.find('@'); at != std::string_view::npos) {
    token = token.substr(0, at);
  }

  if (token == "/start") {
    return BotCommand::kStart;
  }

  if (token == "/menu") {
    return BotCommand::kMenu;
  }

  if (token == "/stop") {
    return BotCommand::kStop;
  }

  if (token == "/stats") {
    return BotCommand::kStats;
  }

  return BotCommand::kUnknown;
}

void DeleteMessageBestEffort(TelegramBot& bot, std::int64_t chat_id, std::int64_t message_id,
                             const std::stop_token& stop_token) {
  try {
    bot.Call("deleteMessage",
             {
                 {"chat_id", chat_id},
                 {"message_id", message_id},
             },
             stop_token);

  } catch (const std::exception& error) {
    spdlog::warn(
        "failed to delete Telegram message {} "
        "in chat {}: {}",
        message_id, chat_id, error.what());
  }
}

const std::vector<AnswerOption>* ChoiceOptions(const Question& question) {
  if (const auto* task = std::get_if<SingleChoiceTask>(&question.task)) {
    return &task->options;
  }

  if (const auto* task = std::get_if<MultipleChoiceTask>(&question.task)) {
    return &task->options;
  }

  return nullptr;
}

bool HasOption(const std::vector<AnswerOption>& options, std::string_view option_id) {
  return std::ranges::find(options, option_id, &AnswerOption::id) != options.end();
}

std::vector<std::string> BuildOptionOrder(const Question& question) {
  const auto* options = ChoiceOptions(question);

  if (options == nullptr) {
    return {};
  }

  std::vector<std::string> order;
  order.reserve(options->size());

  for (const auto& option : *options) {
    order.push_back(option.id);
  }

  static thread_local std::mt19937 generator{std::random_device{}()};

  std::ranges::shuffle(order, generator);

  return order;
}

std::optional<TrainingContentMode> ParseTrainingContentMode(std::string_view payload) {
  if (payload == "mode:theory") {
    return TrainingContentMode::kTheory;
  }

  if (payload == "mode:api") {
    return TrainingContentMode::kApi;
  }

  if (payload == "mode:code") {
    return TrainingContentMode::kCode;
  }

  if (payload == "mode:matching") {
    return TrainingContentMode::kMatching;
  }

  return std::nullopt;
}

std::optional<std::string_view> ParseOptionPayload(std::string_view payload, std::string_view prefix) {
  if (!payload.starts_with(prefix)) {
    return std::nullopt;
  }

  payload.remove_prefix(prefix.size());

  if (payload.empty()) {
    return std::nullopt;
  }

  return payload;
}

struct MatchingCallback {
  MatchingSide side = MatchingSide::kLeft;
  std::size_t revision = 0;
  std::size_t position = 0;
};

std::optional<MatchingCallback> ParseMatchingCallback(std::string_view payload) {
  if (!payload.starts_with("match:")) {
    return std::nullopt;
  }
  payload.remove_prefix(6);
  if (payload.size() < 4 || payload[1] != ':') {
    return std::nullopt;
  }

  MatchingCallback callback;
  if (payload[0] == 'l') {
    callback.side = MatchingSide::kLeft;
  } else if (payload[0] == 'r') {
    callback.side = MatchingSide::kRight;
  } else {
    return std::nullopt;
  }

  payload.remove_prefix(2);
  const auto separator = payload.find(':');
  if (separator == std::string_view::npos) {
    return std::nullopt;
  }

  const auto read_number = [](std::string_view value, std::size_t& output) {
    if (value.empty()) {
      return false;
    }
    const auto [end, error] = std::from_chars(value.data(), value.data() + value.size(), output);
    return error == std::errc{} && end == value.data() + value.size();
  };

  if (!read_number(payload.substr(0, separator), callback.revision) ||
      !read_number(payload.substr(separator + 1), callback.position)) {
    return std::nullopt;
  }
  return callback;
}

std::string SerializeOptionIds(std::span<const std::string> option_ids) {
  std::vector<std::string> sorted(option_ids.begin(), option_ids.end());

  std::ranges::sort(sorted);

  boost::json::array values;
  values.reserve(sorted.size());

  for (const auto& id : sorted) {
    values.emplace_back(id);
  }

  return boost::json::serialize(values);
}

TrainingProgressView BuildProgressView(const TelegramSession& session, const Question& question, int topic_level) {
  return {
      .topic = question.topic,
      .topic_level = topic_level,
      .adaptive_level = session.quiz.adaptive_state.level,
      .question_level = question.level,
      .completed_questions = session.quiz.completed_question_ids.size(),
      .session_goal = session.quiz.session_goal,
      .question_number = session.questions_started,
      .correct_attempts = session.quiz.correct_attempts,
      .total_attempts = session.quiz.total_attempts,
      .current_streak = session.quiz.current_streak,
      .repeat = session.current_question_is_repeat,
      .attempt_number = session.current_attempt,
  };
}

std::string BuildCompletionNotice(const TelegramSession& session) {
  const auto& quiz = session.quiz;

  std::string text = "🎯 Тренировка завершена.\n\n";

  text += "Пройдено: ";
  text += std::to_string(quiz.completed_question_ids.size());
  text += '/';
  text += std::to_string(quiz.session_goal);

  text += "\nОсвоено: ";
  text += std::to_string(quiz.mastered_questions);
  text += '/';
  text += std::to_string(quiz.session_goal);

  text += "\nС первой попытки: ";
  text += std::to_string(quiz.first_try_correct);
  text += '/';
  text += std::to_string(quiz.session_goal);

  text += "\nТочность: ";

  if (quiz.total_attempts == 0) {
    text += "—";
  } else {
    const auto accuracy = (quiz.correct_attempts * 100 + quiz.total_attempts / 2) / quiz.total_attempts;

    text += std::to_string(accuracy);
    text += '%';
  }

  text += "\nЛучшая серия: ";
  text += std::to_string(quiz.best_streak);

  text += "\nУровень: ";
  text += std::to_string(quiz.initial_level);

  if (quiz.initial_level != quiz.adaptive_state.level) {
    text += " → ";
    text += std::to_string(quiz.adaptive_state.level);
  }

  return text;
}

}  // namespace

TelegramUpdateHandler::TelegramUpdateHandler(TelegramBot& bot, QuizSessionService& quiz, AttemptRepository& attempts)
    : bot_(bot), quiz_(quiz), attempts_(attempts) {}

void TelegramUpdateHandler::Handle(const boost::json::object& update, const std::stop_token& stop_token) {
  if (const auto* message = update.if_contains("message"); message != nullptr && message->is_object()) {
    HandleMessage(message->as_object(), stop_token);
    return;
  }

  if (const auto* callback = update.if_contains("callback_query"); callback != nullptr && callback->is_object()) {
    HandleCallback(callback->as_object(), stop_token);
  }
}

void TelegramUpdateHandler::HandleMessage(const boost::json::object& message, const std::stop_token& stop_token) {
  const auto& chat = message.at("chat").as_object();

  if (chat.at("type").as_string() != "private") {
    return;
  }

  const auto chat_id = chat.at("id").as_int64();
  const auto& from = message.at("from").as_object();
  const auto user_id = from.at("id").as_int64();

  const auto* text_value = message.if_contains("text");
  if (text_value == nullptr || !text_value->is_string())
    return;
  const auto& text = text_value->as_string();

  const std::string_view input(text.data(), text.size());

  if (const auto command = ParseBotCommand(input)) {
    switch (*command) {
      case BotCommand::kStart:
        ShowStartMenu(user_id, chat_id, std::nullopt, MenuAction::kStart, stop_token);
        return;

      case BotCommand::kMenu:
        ShowStartMenu(user_id, chat_id, std::nullopt, MenuAction::kMenu, stop_token);
        return;

      case BotCommand::kStop:
        ShowStartMenu(user_id, chat_id, std::nullopt, MenuAction::kStop, stop_token);
        return;

      case BotCommand::kStats:
        ShowProgressSummary(user_id, chat_id, stop_token);
        return;

      case BotCommand::kUnknown:
        return;
    }
  }

  const auto session_it = sessions_.find(chat_id);

  if (session_it == sessions_.end() || session_it->second.user_id != user_id) {
    return;
  }

  HandleTextAnswer(chat_id, message, input, session_it->second, stop_token);
}

void TelegramUpdateHandler::HandleTextAnswer(std::int64_t chat_id, const boost::json::object& message,
                                             std::string_view answer, TelegramSession& session,
                                             const std::stop_token& stop_token) {
  if (session.quiz.answered || !session.message_id) {
    return;
  }

  const auto& question = quiz_.get().CurrentQuestion(session.quiz);

  if (!std::holds_alternative<ShortAnswerTask>(question.task)) {
    return;
  }

  if (const auto* reply = message.if_contains("reply_to_message"); reply != nullptr && reply->is_object()) {
    const auto reply_message_id = reply->as_object().at("message_id").as_int64();

    if (reply_message_id != *session.message_id) {
      return;
    }
  }

  auto submission = quiz_.get().SubmitAnswer(session.quiz, answer);

  const auto display_answer = BuildAnswerDisplayText(question, session.option_order, answer);

  if (!ApplySubmission(chat_id, session, question, std::string(answer), display_answer, std::move(submission),
                       stop_token)) {
    return;
  }

  const auto answer_message_id = message.at("message_id").as_int64();

  DeleteMessageBestEffort(bot_.get(), chat_id, answer_message_id, stop_token);
}

void TelegramUpdateHandler::HandleCallback(const boost::json::object& callback, const std::stop_token& stop_token) {
  const auto* data_value = callback.if_contains("data");

  const auto* message_value = callback.if_contains("message");

  if (data_value == nullptr || !data_value->is_string() || message_value == nullptr || !message_value->is_object()) {
    return;
  }

  const auto& message = message_value->as_object();

  const auto& chat = message.at("chat").as_object();

  if (chat.at("type").as_string() != "private") {
    return;
  }

  const auto chat_id = chat.at("id").as_int64();

  const auto& from = callback.at("from").as_object();

  const auto user_id = from.at("id").as_int64();

  const auto message_id = message.at("message_id").as_int64();

  const auto& data = data_value->as_string();

  const std::string_view payload(data.data(), data.size());

  bot_.get().Call("answerCallbackQuery",
                  {
                      {"callback_query_id", callback.at("id")},
                  },
                  stop_token);

  if (payload == "menu") {
    if (const auto session_it = sessions_.find(chat_id); session_it != sessions_.end()) {
      if (session_it->second.user_id != user_id || !session_it->second.message_id ||
          *session_it->second.message_id != message_id) {
        return;
      }
    }

    if (const auto menu_it = start_menus_.find(chat_id);
        menu_it != start_menus_.end() &&
        (menu_it->second.user_id != user_id || menu_it->second.message_id != message_id)) {
      return;
    }

    ShowStartMenu(user_id, chat_id, message_id, MenuAction::kMenu, stop_token);
    return;
  }

  if (const auto menu_it = start_menus_.find(chat_id);
      menu_it != start_menus_.end() && menu_it->second.user_id == user_id && menu_it->second.message_id == message_id) {
    HandleStartMenuCallback(chat_id, message_id, payload, menu_it->second, stop_token);
    return;
  }

  const auto session_it = sessions_.find(chat_id);

  if (session_it == sessions_.end()) {
    return;
  }

  auto& session = session_it->second;

  if (session.user_id != user_id || !session.message_id || *session.message_id != message_id) {
    return;
  }

  HandleSessionCallback(chat_id, message_id, payload, session, stop_token);
}

void TelegramUpdateHandler::HandleStartMenuCallback(std::int64_t chat_id, std::int64_t message_id,
                                                    std::string_view payload, StartMenuState& menu,
                                                    const std::stop_token& stop_token) {
  if (const auto mode = ParseTrainingContentMode(payload)) {
    ToggleTrainingMode(chat_id, message_id, *mode, menu, stop_token);
    return;
  }

  if (payload == "topics:open") {
    auto request = BuildTopicMenuMessage(chat_id, menu.selected_topics);
    request["message_id"] = message_id;
    bot_.get().Call("editMessageText", std::move(request), stop_token);
    return;
  }

  if (payload == "topics:all") {
    menu.selected_topics.clear();
    bot_.get().Call("editMessageReplyMarkup",
                    {
                        {"chat_id", chat_id},
                        {"message_id", message_id},
                        {"reply_markup", BuildTopicMenuKeyboard(menu.selected_topics)},
                    },
                    stop_token);
    return;
  }

  if (payload == "topics:done") {
    auto request = BuildStartMenuMessage(chat_id, menu.selected_modes, menu.selected_topics);
    request["message_id"] = message_id;
    bot_.get().Call("editMessageText", std::move(request), stop_token);
    return;
  }

  if (const auto topic = ParseOptionPayload(payload, "topic:"); topic && IsKnownTopic(*topic)) {
    ToggleTrainingTopic(chat_id, message_id, *topic, menu, stop_token);
    return;
  }

  if (payload != "training:start" || menu.selected_modes.empty()) {
    return;
  }

  StartTraining(chat_id, menu.selected_modes, menu.selected_topics, stop_token);
}

void TelegramUpdateHandler::HandleSessionCallback(std::int64_t chat_id, std::int64_t message_id,
                                                  std::string_view payload, TelegramSession& session,
                                                  const std::stop_token& stop_token) {
  if (payload == "next") {
    NextQuestion(chat_id, session, stop_token);
    return;
  }

  if (payload == "explain") {
    ShowExplanation(chat_id, message_id, session, stop_token);
    return;
  }

  if (session.quiz.answered) {
    return;
  }

  const auto& question = quiz_.get().CurrentQuestion(session.quiz);

  if (std::holds_alternative<MatchingTask>(question.task)) {
    if (const auto pick = ParseMatchingCallback(payload)) {
      HandleMatchingPick(chat_id, question, pick->side, pick->position, pick->revision, session, stop_token);
    }
    return;
  }

  if (const auto* task = std::get_if<SingleChoiceTask>(&question.task)) {
    const auto option_id = ParseOptionPayload(payload, "answer:");

    if (!option_id || !HasOption(task->options, *option_id)) {
      return;
    }

    auto submission = quiz_.get().SubmitAnswer(session.quiz, *option_id);

    const auto display_answer = BuildAnswerDisplayText(question, session.option_order, *option_id);

    ApplySubmission(chat_id, session, question, std::string(*option_id), display_answer, std::move(submission),
                    stop_token);
    return;
  }

  if (const auto* task = std::get_if<MultipleChoiceTask>(&question.task)) {
    if (const auto option_id = ParseOptionPayload(payload, "toggle:")) {
      if (!HasOption(task->options, *option_id)) {
        return;
      }

      ToggleOption(chat_id, message_id, question, *option_id, session, stop_token);
      return;
    }

    if (payload == "submit") {
      SubmitMultipleChoice(chat_id, question, session, stop_token);
    }
  }
}

bool TelegramUpdateHandler::ApplySubmission(std::int64_t chat_id, TelegramSession& session, const Question& question,
                                            const std::string& storage_answer, const std::string& display_answer,
                                            AnswerSubmission submission, const std::stop_token& stop_token) {
  if (submission.result == AnswerResult::kUnknownOption) {
    return false;
  }

  if (!session.message_id) {
    throw std::logic_error(
        "cannot apply submission without "
        "active Telegram message");
  }

  TelegramSession next = session;

  next.quiz = std::move(submission.session);

  const bool correct = submission.result == AnswerResult::kCorrect;

  const auto previous_level = session.quiz.adaptive_state.level;

  const auto next_level = next.quiz.adaptive_state.level;

  const auto previous_topic_level = quiz_.get().TopicLevel(session.quiz, question.topic);

  const auto next_topic_state = quiz_.get().TopicState(next.quiz, question.topic);

  const auto next_topic_level = next_topic_state.level;

  const auto record_result = attempts_.get().RecordSubmission(SubmissionRecord{
      .user_id = session.user_id,
      .session_id = session.storage_session_id,
      .question_id = question.id,
      .topic = question.topic,
      .skill_id = question.skill_id,
      .answer = storage_answer,
      .correct = correct,
      .completed = submission.completed,
      .attempt_number = submission.attempts,
      .topic_level = next_topic_level,
      .topic_level_evidence = next_topic_state.evidence,
      .adaptive_level = next_level,
      .adaptive_level_evidence = next.quiz.adaptive_state.evidence,
  });

  if (record_result == RecordSubmissionResult::kDuplicate) {
    spdlog::warn("duplicate answer submission ignored, session_id={}, question_id={}, attempt={}",
                 session.storage_session_id, question.id, submission.attempts);
  }

  if (submission.completed) {
    if (previous_topic_level != next_topic_level) {
      spdlog::info("topic level changed, user_id={}, topic={}, {} -> {}", session.user_id, question.topic,
                   previous_topic_level, next_topic_level);
    }

    if (previous_level != next_level) {
      spdlog::info("adaptive level changed, user_id={}, {} -> {}", session.user_id, previous_level, next_level);
    }
  }

  next.selected_option_ids.clear();

  const bool offer_explanation = !correct && submission.completed;

  if (offer_explanation) {
    next.pending_explanation_question_id = question.id;

    next.pending_explanation_answer_text = display_answer;

  } else {
    next.pending_explanation_question_id.reset();
    next.pending_explanation_answer_text.reset();
  }

  auto feedback_message = boost::json::object{
      {"chat_id", chat_id},
      {"text", BuildAnswerText(question, session.option_order, display_answer,
                               AnswerFeedbackView{
                                   .result = submission.result,
                                   .attempts = submission.attempts,
                                   .completed = submission.completed,
                                   .show_explanation = correct,
                                   .previous_topic_level = previous_topic_level,
                                   .previous_adaptive_level = previous_level,
                               },
                               BuildProgressView(next, question, quiz_.get().TopicLevel(next.quiz, question.topic)))},
      {"parse_mode", "HTML"},
      {"reply_markup", BuildAnswerKeyboard(offer_explanation)},
  };

  feedback_message["message_id"] = *session.message_id;
  bot_.get().Call("editMessageText", feedback_message, stop_token);

  session = std::move(next);

  return true;
}

void TelegramUpdateHandler::ToggleOption(std::int64_t chat_id, std::int64_t message_id, const Question& question,
                                         std::string_view option_id, TelegramSession& session,
                                         const std::stop_token& stop_token) {
  const auto it = std::ranges::find(session.selected_option_ids, option_id);

  if (it == session.selected_option_ids.end()) {
    session.selected_option_ids.emplace_back(option_id);
  } else {
    session.selected_option_ids.erase(it);
  }

  bot_.get().Call(
      "editMessageReplyMarkup",
      {
          {"chat_id", chat_id},
          {"message_id", message_id},
          {"reply_markup", BuildQuestionKeyboard(question, session.option_order, session.selected_option_ids)},
      },
      stop_token);
}

void TelegramUpdateHandler::SubmitMultipleChoice(std::int64_t chat_id, const Question& question,
                                                 TelegramSession& session, const std::stop_token& stop_token) {
  if (session.selected_option_ids.empty()) {
    return;
  }

  const std::span<const std::string> answers{session.selected_option_ids};

  auto submission = quiz_.get().SubmitAnswer(session.quiz, answers);

  const auto storage_answer = SerializeOptionIds(answers);

  const auto display_answer = BuildAnswerDisplayText(question, session.option_order, answers);

  ApplySubmission(chat_id, session, question, storage_answer, display_answer, std::move(submission), stop_token);
}

void TelegramUpdateHandler::HandleMatchingPick(std::int64_t chat_id, const Question& question, MatchingSide side,
                                               std::size_t position, std::size_t revision, TelegramSession& session,
                                               const std::stop_token& stop_token) {
  if (!session.matching || !session.message_id) {
    throw std::logic_error("matching callback without matching state");
  }

  MatchingRound next_round = *session.matching;
  const auto outcome = next_round.Select(side, position, revision);
  if (outcome == MatchingPick::kIgnored) {
    return;
  }

  if (outcome == MatchingPick::kFinished) {
    const auto* task = std::get_if<MatchingTask>(&question.task);
    if (task == nullptr) {
      throw std::logic_error("matching state for unsupported task");
    }

    const auto storage_answer = boost::json::serialize(boost::json::object{
        {"matched", next_round.MatchedCount()},
        {"mistakes", next_round.Mistakes()},
    });
    const auto display_answer = std::to_string(next_round.MatchedCount()) + "/" + std::to_string(task->pairs.size()) +
                                " пар, ошибок: " + std::to_string(next_round.Mistakes());
    auto submission = quiz_.get().SubmitMatchingAnswer(session.quiz, next_round.Mistakes() == 0);
    ApplySubmission(chat_id, session, question, storage_answer, display_answer, std::move(submission), stop_token);
    return;
  }

  auto message = BuildQuestionMessage(
      chat_id, question, session.option_order, session.selected_option_ids,
      BuildProgressView(session, question, quiz_.get().TopicLevel(session.quiz, question.topic)), &next_round);
  message["message_id"] = *session.message_id;
  bot_.get().Call("editMessageText", message, stop_token);
  session.matching = std::move(next_round);
}

void TelegramUpdateHandler::ShowExplanation(std::int64_t chat_id, std::int64_t message_id, TelegramSession& session,
                                            const std::stop_token& stop_token) {
  if (!session.pending_explanation_question_id || !session.pending_explanation_answer_text) {
    return;
  }

  const auto& question = quiz_.get().GetQuestion(*session.pending_explanation_question_id);

  bot_.get().Call(
      "editMessageText",
      {
          {"chat_id", chat_id},
          {"message_id", message_id},
          {"text",
           BuildAnswerText(question, session.option_order, *session.pending_explanation_answer_text,
                           AnswerFeedbackView{
                               .result = AnswerResult::kIncorrect,
                               .attempts = session.current_attempt,
                               .completed = true,
                               .show_explanation = true,
                               .previous_topic_level = quiz_.get().TopicLevel(session.quiz, question.topic),
                               .previous_adaptive_level = session.quiz.adaptive_state.level,
                           },
                           BuildProgressView(session, question, quiz_.get().TopicLevel(session.quiz, question.topic)))},
          {"parse_mode", "HTML"},
          {"reply_markup", BuildAnswerKeyboard(false)},
      },
      stop_token);

  session.pending_explanation_question_id.reset();
  session.pending_explanation_answer_text.reset();
}

void TelegramUpdateHandler::ShowStartMenu(std::int64_t user_id, std::int64_t chat_id,
                                          std::optional<std::int64_t> source_message_id, MenuAction action,
                                          const std::stop_token& stop_token) {
  attempts_.get().EnsureUser(user_id);

  std::string_view finish_reason;
  std::string notice;

  switch (action) {
    case MenuAction::kStart:
      finish_reason = "restarted";
      break;

    case MenuAction::kMenu:
      finish_reason = "menu";
      break;

    case MenuAction::kStop:
      finish_reason = "stopped";
      notice = "Тренировка остановлена.";
      break;

    case MenuAction::kCompleted:
      finish_reason = "completed";
      notice = "Тренировка завершена.";
      break;
  }

  std::optional<std::int64_t> previous_message_id = source_message_id;

  if (const auto session_it = sessions_.find(chat_id); session_it != sessions_.end()) {
    if (session_it->second.user_id != user_id) {
      return;
    }

    previous_message_id = session_it->second.message_id;

    if (action == MenuAction::kCompleted) {
      notice = BuildCompletionNotice(session_it->second);
    }

    FinishStorageSession(session_it->second, finish_reason);

    sessions_.erase(session_it);

  } else if (action != MenuAction::kCompleted) {
    const auto orphan_reason =
        action == MenuAction::kStop ? std::string_view{"stopped"} : std::string_view{"interrupted"};

    attempts_.get().FinishOpenSessionsWithModePrefix(user_id, "training:", orphan_reason);
  }

  if (const auto menu_it = start_menus_.find(chat_id); menu_it != start_menus_.end()) {
    if (menu_it->second.user_id != user_id) {
      return;
    }

    previous_message_id = menu_it->second.message_id;
  }

  std::vector<TrainingContentMode> selected_modes;
  std::vector<std::string> selected_topics;

  const auto new_message_id =
      SendReplacingMessage(chat_id, previous_message_id,
                           BuildStartMenuMessage(chat_id, selected_modes, selected_topics, notice), stop_token);

  start_menus_.insert_or_assign(chat_id, StartMenuState{
                                             .user_id = user_id,
                                             .message_id = new_message_id,
                                             .selected_modes = {},
                                             .selected_topics = {},
                                         });
}

void TelegramUpdateHandler::ShowProgressSummary(std::int64_t user_id, std::int64_t chat_id,
                                                const std::stop_token& stop_token) {
  const auto summary = attempts_.get().LoadProgressSummary(user_id);
  bot_.get().Call("sendMessage", BuildProgressSummaryMessage(chat_id, summary), stop_token);
}

void TelegramUpdateHandler::ToggleTrainingMode(std::int64_t chat_id, std::int64_t message_id, TrainingContentMode mode,
                                               StartMenuState& menu, const std::stop_token& stop_token) {
  const auto it = std::ranges::find(menu.selected_modes, mode);

  if (it == menu.selected_modes.end()) {
    if (mode == TrainingContentMode::kMatching) {
      menu.selected_modes.clear();
    } else {
      std::erase(menu.selected_modes, TrainingContentMode::kMatching);
    }
    menu.selected_modes.push_back(mode);
  } else {
    menu.selected_modes.erase(it);
  }

  bot_.get().Call("editMessageReplyMarkup",
                  {
                      {"chat_id", chat_id},
                      {"message_id", message_id},
                      {"reply_markup", BuildStartMenuKeyboard(menu.selected_modes, menu.selected_topics)},
                  },
                  stop_token);
}

void TelegramUpdateHandler::ToggleTrainingTopic(std::int64_t chat_id, std::int64_t message_id, std::string_view topic,
                                                StartMenuState& menu, const std::stop_token& stop_token) {
  const auto it = std::ranges::find(menu.selected_topics, topic);

  if (it == menu.selected_topics.end()) {
    menu.selected_topics.emplace_back(topic);
  } else {
    menu.selected_topics.erase(it);
  }

  bot_.get().Call("editMessageReplyMarkup",
                  {
                      {"chat_id", chat_id},
                      {"message_id", message_id},
                      {"reply_markup", BuildTopicMenuKeyboard(menu.selected_topics)},
                  },
                  stop_token);
}

void TelegramUpdateHandler::StartTraining(std::int64_t chat_id, const std::vector<TrainingContentMode>& modes,
                                          const std::vector<std::string>& topics, const std::stop_token& stop_token) {
  if (modes.empty()) {
    return;
  }

  const auto menu_it = start_menus_.find(chat_id);

  if (menu_it == start_menus_.end()) {
    return;
  }

  const auto user_id = menu_it->second.user_id;

  const auto selection = QuestionSelectionForModes(modes, topics);

  if (!quiz_.get().HasQuestions(selection)) {
    auto request =
        BuildStartMenuMessage(chat_id, modes, topics, "Для такого сочетания тем и режимов пока нет вопросов.");
    request["message_id"] = menu_it->second.message_id;
    bot_.get().Call("editMessageText", std::move(request), stop_token);
    return;
  }

  const auto storage_mode = TrainingModeStorageName(modes);

  const auto adaptive_state = attempts_.get().GetAdaptiveState(user_id);

  auto topic_states = attempts_.get().LoadTopicStates(user_id);

  auto review_states = attempts_.get().LoadReviewStates(user_id);

  TelegramSession session{
      .user_id = user_id,
      .quiz = quiz_.get().Start(selection, adaptive_state, std::move(topic_states), std::move(review_states)),
      .storage_session_id = attempts_.get().StartSession(user_id, storage_mode),
      .message_id = menu_it->second.message_id,
      .questions_started = 0,
      .current_question_is_repeat = false,
      .current_attempt = 1,
      .option_order = {},
      .selected_option_ids = {},
      .matching = std::nullopt,
      .pending_explanation_question_id = std::nullopt,
      .pending_explanation_answer_text = std::nullopt,
  };

  try {
    SendQuestion(chat_id, session, stop_token);

  } catch (...) {
    FinishStorageSession(session, "start_failed");
    throw;
  }

  start_menus_.erase(menu_it);

  sessions_.insert_or_assign(chat_id, std::move(session));
}

void TelegramUpdateHandler::SendQuestion(std::int64_t chat_id, TelegramSession& session,
                                         const std::stop_token& stop_token) {
  const auto& question = quiz_.get().CurrentQuestion(session.quiz);

  auto option_order = BuildOptionOrder(question);

  std::optional<MatchingRound> matching;
  if (const auto* task = std::get_if<MatchingTask>(&question.task)) {
    matching.emplace(task->pairs.size());
  }

  session.selected_option_ids.clear();

  session.current_attempt = session.quiz.queue.CurrentAttempts() + 1;
  session.current_question_is_repeat = session.current_attempt > 1;

  if (!session.current_question_is_repeat) {
    ++session.questions_started;
  }

  auto message =
      BuildQuestionMessage(chat_id, question, option_order, session.selected_option_ids,
                           BuildProgressView(session, question, quiz_.get().TopicLevel(session.quiz, question.topic)),
                           matching ? &*matching : nullptr);

  const auto new_message_id = SendReplacingMessage(chat_id, session.message_id, std::move(message), stop_token);

  session.message_id = new_message_id;
  session.option_order = std::move(option_order);
  session.matching = std::move(matching);
  session.quiz.answered = false;
  session.pending_explanation_question_id.reset();
  session.pending_explanation_answer_text.reset();
}

void TelegramUpdateHandler::NextQuestion(std::int64_t chat_id, TelegramSession& session,
                                         const std::stop_token& stop_token) {
  if (!session.quiz.answered) {
    return;
  }

  if (!session.quiz.queue.Empty()) {
    SendQuestion(chat_id, session, stop_token);
    return;
  }

  session.quiz = quiz_.get().Refill(session.quiz);

  if (!session.quiz.queue.Empty()) {
    SendQuestion(chat_id, session, stop_token);
    return;
  }

  ShowStartMenu(session.user_id, chat_id, session.message_id, MenuAction::kCompleted, stop_token);
}

std::int64_t TelegramUpdateHandler::SendReplacingMessage(std::int64_t chat_id,
                                                         std::optional<std::int64_t> previous_message_id,
                                                         boost::json::object message,
                                                         const std::stop_token& stop_token) {
  const auto response = bot_.get().Call("sendMessage", message, stop_token);

  const auto new_message_id = response.as_object().at("message_id").as_int64();

  if (previous_message_id && *previous_message_id != new_message_id) {
    DeleteMessageBestEffort(bot_.get(), chat_id, *previous_message_id, stop_token);
  }

  return new_message_id;
}

void TelegramUpdateHandler::FinishStorageSession(TelegramSession& session, std::string_view finish_reason) {
  if (session.storage_session_id == 0) {
    return;
  }

  attempts_.get().FinishSession(session.storage_session_id, finish_reason);

  session.storage_session_id = 0;
}

void TelegramUpdateHandler::FinishActiveSessions() {
  for (auto& [chat_id, session] : sessions_) {
    if (session.storage_session_id == 0) {
      continue;
    }

    try {
      FinishStorageSession(session, "shutdown");

    } catch (const std::exception& error) {
      spdlog::warn(
          "failed to finish storage session {} "
          "for chat {} during shutdown: {}",
          session.storage_session_id, chat_id, error.what());
    }
  }
}
