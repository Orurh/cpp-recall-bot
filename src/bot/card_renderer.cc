#include "card_renderer.h"

#include "quiz/topic_catalog.h"

#include <boost/json.hpp>

#include <algorithm>
#include <cstddef>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace {

std::string EscapeHtml(std::string_view text) {
  std::string result;
  result.reserve(text.size());

  for (char ch : text) {
    switch (ch) {
      case '&':
        result += "&amp;";
        break;
      case '<':
        result += "&lt;";
        break;
      case '>':
        result += "&gt;";
        break;
      case '"':
        result += "&quot;";
        break;
      default:
        result += ch;
        break;
    }
  }

  return result;
}

std::string OptionLabel(std::size_t index) {
  constexpr std::string_view kLabels = "ABCDEFGHIJKLMNOPQRSTUVWXYZ";

  if (index >= kLabels.size()) {
    throw std::invalid_argument("too many answer options");
  }

  return std::string(1, kLabels[index]);
}

void AddButton(boost::json::array& keyboard, std::string_view text, std::string_view callback_data) {
  boost::json::array row;
  row.push_back(boost::json::object{
      {"text", text},
      {"callback_data", callback_data},
  });

  keyboard.push_back(std::move(row));
}

void AddChoiceButton(boost::json::array& keyboard, std::string text, std::string callback_data) {
  constexpr std::size_t kButtonsPerRow = 4;

  if (keyboard.empty() || keyboard.back().as_array().size() >= kButtonsPerRow) {
    keyboard.push_back(boost::json::array{});
  }

  keyboard.back().as_array().push_back(boost::json::object{
      {"text", std::move(text)},
      {"callback_data", std::move(callback_data)},
  });
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

const AnswerOption& FindOption(const std::vector<AnswerOption>& options, std::string_view option_id) {
  const auto it = std::ranges::find(options, option_id, &AnswerOption::id);

  if (it == options.end()) {
    throw std::logic_error("unknown option id: " + std::string(option_id));
  }

  return *it;
}

void ValidateOptionOrder(const std::vector<AnswerOption>& options, const std::vector<std::string>& option_order) {
  if (option_order.size() != options.size()) {
    throw std::logic_error(
        "option_order size does not match "
        "options size");
  }

  for (const auto& option_id : option_order) {
    static_cast<void>(FindOption(options, option_id));
  }
}

bool IsSelected(const std::vector<std::string>& selected_option_ids, std::string_view option_id) {
  return std::ranges::find(selected_option_ids, option_id) != selected_option_ids.end();
}

bool IsTopicSelected(std::span<const std::string> selected_topics, std::string_view topic_id) {
  return std::ranges::find(selected_topics, topic_id) != selected_topics.end();
}

void AppendOptions(std::string& text, const std::vector<AnswerOption>& options,
                   const std::vector<std::string>& option_order) {
  ValidateOptionOrder(options, option_order);

  text += "\n\n";

  for (std::size_t i = 0; i < option_order.size(); ++i) {
    const auto& option = FindOption(options, option_order[i]);

    text += "<b>";
    text += OptionLabel(i);
    text += ".</b> ";
    text += EscapeHtml(option.text);

    if (i + 1 != option_order.size()) {
      text += '\n';
    }
  }
}

std::string OptionLabelForId(const std::vector<std::string>& option_order, std::string_view option_id) {
  const auto it = std::ranges::find(option_order, option_id);

  if (it == option_order.end()) {
    throw std::logic_error(
        "answer references option outside "
        "option_order: " +
        std::string(option_id));
  }

  return OptionLabel(static_cast<std::size_t>(std::distance(option_order.begin(), it)));
}

void ValidateCallbackData(std::string_view callback_data) {
  if (callback_data.size() > 64) {
    throw std::invalid_argument("callback_data exceeds 64 bytes");
  }
}

std::string TopicDisplayName(std::string_view topic) {
  if (topic.empty()) {
    throw std::invalid_argument("training topic must not be empty");
  }

  if (const auto title = TopicTitle(topic)) {
    return std::string(*title);
  }

  std::string result(topic);

  std::ranges::replace(result, '_', ' ');

  if (result.front() >= 'a' && result.front() <= 'z') {
    result.front() = static_cast<char>(result.front() - 'a' + 'A');
  }

  return result;
}

std::string BuildProgressBar(std::size_t completed, std::size_t goal) {
  if (goal == 0 || completed > goal) {
    throw std::invalid_argument("invalid training progress");
  }

  std::string result;

  for (std::size_t i = 0; i < goal; ++i) {
    result += i < completed ? "●" : "○";
  }

  return result;
}

std::string BuildTrainingHeader(const TrainingProgressView& progress) {
  if (progress.session_goal == 0 || progress.completed_questions > progress.session_goal) {
    throw std::invalid_argument("invalid training progress view");
  }

  if (progress.topic_level < 1 || progress.topic_level > 10 || progress.adaptive_level < 1 ||
      progress.adaptive_level > 10 || progress.question_level < 1 || progress.question_level > 10) {
    throw std::invalid_argument("invalid training level");
  }

  if (progress.attempt_number < 1 || progress.attempt_number > 3) {
    throw std::invalid_argument("invalid training attempt number");
  }

  std::string text = "<b>🧠 ";
  text += EscapeHtml(TopicDisplayName(progress.topic));
  text += "</b>\nТема: ур. ";
  text += std::to_string(progress.topic_level);
  text += " · вопрос: ур. ";
  text += std::to_string(progress.question_level);
  text += " · общий: ур. ";
  text += std::to_string(progress.adaptive_level);
  text += '\n';

  if (progress.repeat) {
    text += "🔁 Повтор · попытка ";
    text += std::to_string(progress.attempt_number);
    text += "/3";
  } else {
    if (progress.question_number == 0 || progress.question_number > progress.session_goal) {
      throw std::invalid_argument("invalid training question number");
    }

    text += "Вопрос ";
    text += std::to_string(progress.question_number);
    text += '/';
    text += std::to_string(progress.session_goal);
  }

  text += " · ";
  text += BuildProgressBar(progress.completed_questions, progress.session_goal);
  text += ' ';
  text += std::to_string(progress.completed_questions);
  text += '/';
  text += std::to_string(progress.session_goal);

  text += "\n🎯 ";

  if (progress.total_attempts == 0) {
    text += "—";
  } else {
    const auto accuracy = ((progress.correct_attempts * 100) + (progress.total_attempts / 2)) / progress.total_attempts;

    text += std::to_string(accuracy);
    text += '%';
  }

  text += " · 🔥 ";
  text += std::to_string(progress.current_streak);

  return text;
}

std::int64_t Percentage(std::int64_t part, std::int64_t total) {
  if (part < 0 || total <= 0 || part > total) {
    throw std::invalid_argument("invalid progress ratio");
  }

  return (part * 100 + total / 2) / total;
}

std::string EvidenceDisplay(int evidence) {
  if (evidence < -3 || evidence > 3) {
    throw std::invalid_argument("invalid adaptive evidence");
  }

  if (evidence > 0) {
    return "⬆️ " + std::to_string(evidence) + "/4";
  }

  if (evidence < 0) {
    return "⬇️ " + std::to_string(-evidence) + "/4";
  }

  return "• 0/4";
}

void ValidateTopicSummary(const TopicProgressSummary& topic) {
  if (topic.topic.empty() || topic.adaptive_level < 1 || topic.adaptive_level > 10 || topic.adaptive_evidence < -3 ||
      topic.adaptive_evidence > 3 || topic.completed_count < 0 || topic.correct_count < 0 || topic.failed_count < 0 ||
      topic.first_try_correct_count < 0 || topic.correct_count + topic.failed_count != topic.completed_count ||
      topic.first_try_correct_count > topic.correct_count) {
    throw std::invalid_argument("invalid topic progress summary");
  }
}

void ValidateProgressSummary(const UserProgressSummary& summary) {
  if (summary.adaptive_level < 1 || summary.adaptive_level > 10 || summary.adaptive_evidence < -3 ||
      summary.adaptive_evidence > 3 || summary.sessions_started < 0 || summary.sessions_completed < 0 ||
      summary.sessions_completed > summary.sessions_started || summary.questions_seen < 0 ||
      summary.completed_count < 0 || summary.correct_count < 0 || summary.failed_count < 0 ||
      summary.first_try_correct_count < 0 || summary.due_review_count < 0 ||
      summary.correct_count + summary.failed_count != summary.completed_count ||
      summary.first_try_correct_count > summary.correct_count || summary.due_review_count > summary.questions_seen) {
    throw std::invalid_argument("invalid user progress summary");
  }

  for (const auto& topic : summary.topics) {
    ValidateTopicSummary(topic);
  }
}

}  // namespace

boost::json::object BuildStartMenuKeyboard(std::span<const TrainingContentMode> selected_modes,
                                           std::span<const std::string> selected_topics) {
  boost::json::array keyboard;

  std::string topic_button = "📚 Темы: ";

  if (selected_topics.empty()) {
    topic_button += "все";
  } else if (selected_topics.size() == 1) {
    topic_button += TopicDisplayName(selected_topics.front());
  } else {
    topic_button += std::to_string(selected_topics.size());
  }

  AddButton(keyboard, topic_button, "topics:open");

  const auto add_mode = [&keyboard, selected_modes](TrainingContentMode mode, std::string_view label,
                                                    std::string_view callback_data) {
    std::string button_text;

    if (ContainsTrainingMode(selected_modes, mode)) {
      button_text = "✓ ";
    }

    button_text += label;

    AddButton(keyboard, button_text, callback_data);
  };

  add_mode(TrainingContentMode::kTheory, "Теория", "mode:theory");

  add_mode(TrainingContentMode::kApi, "API / STL", "mode:api");

  add_mode(TrainingContentMode::kCode, "Код", "mode:code");

  add_mode(TrainingContentMode::kMatching, "🧩 Сопоставление", "mode:matching");

  if (!selected_modes.empty()) {
    AddButton(keyboard, "Начать", "training:start");
  }

  return {
      {"inline_keyboard", std::move(keyboard)},
  };
}

boost::json::object BuildStartMenuMessage(std::int64_t chat_id, std::span<const TrainingContentMode> selected_modes,
                                          std::span<const std::string> selected_topics, std::string_view notice) {
  std::string text;

  if (!notice.empty()) {
    text += notice;
    text += "\n\n";
  }

  text +=
      "Что тренируем?\n\n"
      "Сначала выбери темы, затем один или несколько режимов.\n"
      "Сложность подбирается автоматически.";

  return {
      {"chat_id", chat_id},
      {"text", std::move(text)},
      {"reply_markup", BuildStartMenuKeyboard(selected_modes, selected_topics)},
  };
}

boost::json::object BuildTopicMenuKeyboard(std::span<const std::string> selected_topics) {
  boost::json::array keyboard;

  AddButton(keyboard, selected_topics.empty() ? "✓ Все темы" : "Все темы", "topics:all");

  for (const auto& topic : TrainingTopics()) {
    std::string title;

    if (IsTopicSelected(selected_topics, topic.id)) {
      title = "✓ ";
    }

    title += topic.title;

    AddButton(keyboard, title, "topic:" + std::string(topic.id));
  }

  AddButton(keyboard, "Готово →", "topics:done");

  return {
      {"inline_keyboard", std::move(keyboard)},
  };
}

boost::json::object BuildTopicMenuMessage(std::int64_t chat_id, std::span<const std::string> selected_topics) {
  std::string text = "Какие темы тренируем?\n\n";

  if (selected_topics.empty()) {
    text += "Сейчас выбраны все темы.";
  } else {
    text += "Выбрано тем: ";
    text += std::to_string(selected_topics.size());
    text += ". Можно отметить несколько.";
  }

  return {
      {"chat_id", chat_id},
      {"text", std::move(text)},
      {"reply_markup", BuildTopicMenuKeyboard(selected_topics)},
  };
}

boost::json::object BuildProgressSummaryMessage(std::int64_t chat_id, const UserProgressSummary& summary) {
  ValidateProgressSummary(summary);

  std::string text = "<b>📊 Статистика</b>\n\nОбщий уровень: ";
  text += std::to_string(summary.adaptive_level);
  text += " · шкала ";
  text += EvidenceDisplay(summary.adaptive_evidence);
  text += "\nТренировок завершено: ";
  text += std::to_string(summary.sessions_completed);
  text += '/';
  text += std::to_string(summary.sessions_started);
  text += "\nВопросов изучено: ";
  text += std::to_string(summary.questions_seen);
  text += "\nКарточек завершено: ";
  text += std::to_string(summary.completed_count);
  text += "\nОсвоено: ";
  text += std::to_string(summary.correct_count);
  text += "\nС первой попытки: ";
  text += std::to_string(summary.first_try_correct_count);
  text += "\nК повторению сейчас: ";
  text += std::to_string(summary.due_review_count);

  if (summary.completed_count > 0) {
    text += "\nТочность: ";
    text += std::to_string(Percentage(summary.correct_count, summary.completed_count));
    text += '%';
  }

  if (summary.topics.empty()) {
    text += "\n\nПока нет завершённых карточек.";
  } else {
    text += "\n\n<b>По темам</b>";

    for (const auto& topic : summary.topics) {
      text += "\n• ";
      text += EscapeHtml(TopicDisplayName(topic.topic));
      text += " — ур. ";
      text += std::to_string(topic.adaptive_level);
      text += " · ";
      text += EvidenceDisplay(topic.adaptive_evidence);
      text += " · ";
      text += std::to_string(topic.correct_count);
      text += '/';
      text += std::to_string(topic.completed_count);

      if (topic.completed_count > 0) {
        text += " (";
        text += std::to_string(Percentage(topic.correct_count, topic.completed_count));
        text += "%)";
      }
    }
  }

  return {
      {"chat_id", chat_id},
      {"text", std::move(text)},
      {"parse_mode", "HTML"},
  };
}

boost::json::object BuildQuestionKeyboard(const Question& question, const std::vector<std::string>& option_order,
                                          const std::vector<std::string>& selected_option_ids,
                                          const MatchingRound* matching) {
  boost::json::array keyboard;

  if (const auto* task = std::get_if<MatchingTask>(&question.task)) {
    if (matching == nullptr || matching->Size() != task->pairs.size()) {
      throw std::logic_error("matching question missing round state");
    }

    std::vector<std::size_t> left_positions;
    std::vector<std::size_t> right_positions;
    for (std::size_t position = 0; position < matching->Size(); ++position) {
      if (!matching->IsMatched(matching->PairIndex(MatchingSide::kLeft, position))) {
        left_positions.push_back(position);
      }
      if (!matching->IsMatched(matching->PairIndex(MatchingSide::kRight, position))) {
        right_positions.push_back(position);
      }
    }

    for (std::size_t row_index = 0; row_index < left_positions.size(); ++row_index) {
      boost::json::array row;
      for (const auto side : {MatchingSide::kLeft, MatchingSide::kRight}) {
        const auto position = side == MatchingSide::kLeft ? left_positions[row_index] : right_positions[row_index];
        const auto pair_index = matching->PairIndex(side, position);
        std::string label;
        if (matching->IsSelected(side, pair_index)) {
          label += "🔹 ";
        }
        label += side == MatchingSide::kLeft ? task->pairs[pair_index].left : task->pairs[pair_index].right;
        const std::string data = std::string(side == MatchingSide::kLeft ? "match:l:" : "match:r:") +
                                 std::to_string(matching->Revision()) + ":" + std::to_string(position);
        ValidateCallbackData(data);
        row.emplace_back(boost::json::object{
            {"text", std::move(label)},
            {"callback_data", data},
        });
      }
      keyboard.push_back(std::move(row));
    }
  } else if (const auto* single_task = std::get_if<SingleChoiceTask>(&question.task)) {
    ValidateOptionOrder(single_task->options, option_order);

    for (std::size_t i = 0; i < option_order.size(); ++i) {
      const auto& option_id = option_order[i];

      std::string callback_data = "answer:" + option_id;

      ValidateCallbackData(callback_data);

      AddChoiceButton(keyboard, OptionLabel(i), std::move(callback_data));
    }

  } else if (const auto* multiple_task = std::get_if<MultipleChoiceTask>(&question.task)) {
    ValidateOptionOrder(multiple_task->options, option_order);

    for (std::size_t i = 0; i < option_order.size(); ++i) {
      const auto& option_id = option_order[i];

      std::string button_text;

      if (IsSelected(selected_option_ids, option_id)) {
        button_text = "✓ ";
      }

      button_text += OptionLabel(i);

      std::string callback_data = "toggle:" + option_id;

      ValidateCallbackData(callback_data);

      AddChoiceButton(keyboard, std::move(button_text), std::move(callback_data));
    }

    if (!selected_option_ids.empty()) {
      AddButton(keyboard, "Ответить", "submit");
    }
  }

  AddButton(keyboard, "Главное меню", "menu");

  return {
      {"inline_keyboard", std::move(keyboard)},
  };
}

boost::json::object BuildQuestionMessage(std::int64_t chat_id, const Question& question,
                                         const std::vector<std::string>& option_order,
                                         const std::vector<std::string>& selected_option_ids,
                                         const TrainingProgressView& progress, const MatchingRound* matching) {
  std::string text = BuildTrainingHeader(progress);
  text += "\n\n";
  text += EscapeHtml(question.text);

  if (question.code) {
    text += "\n\n<pre><code class=\"language-cpp\">";
    text += EscapeHtml(*question.code);
    text += "</code></pre>";
  }

  boost::json::object message{
      {"chat_id", chat_id},
      {"parse_mode", "HTML"},
  };

  if (const auto* options = ChoiceOptions(question)) {
    AppendOptions(text, *options, option_order);

    if (std::holds_alternative<MultipleChoiceTask>(question.task)) {
      text += "\n\nМожно выбрать несколько вариантов.";
    }

  } else if (std::holds_alternative<ShortAnswerTask>(question.task)) {
    text += "\n\nНапиши ответ сообщением.";
  } else if (std::holds_alternative<MatchingTask>(question.task)) {
    if (matching == nullptr) {
      throw std::logic_error("matching round not initialized");
    }
    text += "\n\nНайдено пар: ";
    text += std::to_string(matching->MatchedCount());
    text += '/';
    text += std::to_string(matching->Size());
    text += " · Ошибок: ";
    text += std::to_string(matching->Mistakes());
    if (matching->LastMismatch()) {
      text += "\n❌ Не пара. Попробуй ещё.";
    } else {
      text += "\nВыбирай элементы из разных колонок.";
    }
  }

  message["reply_markup"] = BuildQuestionKeyboard(question, option_order, selected_option_ids, matching);

  message["text"] = std::move(text);

  return message;
}

boost::json::object BuildAnswerKeyboard(bool show_explanation_button) {
  boost::json::array keyboard;

  if (show_explanation_button) {
    AddButton(keyboard, "Показать объяснение", "explain");
  }

  AddButton(keyboard, "Следующий вопрос →", "next");

  AddButton(keyboard, "Главное меню", "menu");

  return {
      {"inline_keyboard", std::move(keyboard)},
  };
}

std::string BuildAnswerDisplayText(const Question& question, const std::vector<std::string>& option_order,
                                   std::string_view answer) {
  if (std::holds_alternative<SingleChoiceTask>(question.task)) {
    return OptionLabelForId(option_order, answer);
  }

  if (std::holds_alternative<ShortAnswerTask>(question.task)) {
    return std::string(answer);
  }

  throw std::logic_error(
      "string answer used for unsupported "
      "task type");
}

std::string BuildAnswerDisplayText(const Question& question, const std::vector<std::string>& option_order,
                                   std::span<const std::string> answers) {
  if (!std::holds_alternative<MultipleChoiceTask>(question.task)) {
    throw std::logic_error(
        "multiple answer used for unsupported "
        "task type");
  }

  std::string result;

  for (const auto& option_id : option_order) {
    if (std::ranges::find(answers, option_id) == answers.end()) {
      continue;
    }

    if (!result.empty()) {
      result += ", ";
    }

    result += OptionLabelForId(option_order, option_id);
  }

  return result;
}

std::string BuildAnswerText(const Question& question, const std::vector<std::string>& option_order,
                            std::string_view answer_display, const AnswerFeedbackView& feedback,
                            const TrainingProgressView& progress) {
  if (feedback.result == AnswerResult::kUnknownOption || feedback.attempts < 1 || feedback.attempts > 3 ||
      feedback.previous_topic_level < 1 || feedback.previous_topic_level > 10 || feedback.previous_adaptive_level < 1 ||
      feedback.previous_adaptive_level > 10) {
    throw std::invalid_argument("invalid answer feedback view");
  }

  if (feedback.result == AnswerResult::kIncorrect &&
      ((feedback.completed && feedback.attempts != 3) || (!feedback.completed && feedback.attempts == 3))) {
    throw std::invalid_argument("incorrect answer completion does not match attempt count");
  }

  std::string text = BuildTrainingHeader(progress);
  text += "\n\n";
  text += EscapeHtml(question.text);

  if (question.code) {
    text += "\n\n<pre><code class=\"language-cpp\">";
    text += EscapeHtml(*question.code);
    text += "</code></pre>";
  }

  if (const auto* options = ChoiceOptions(question)) {
    AppendOptions(text, *options, option_order);
  }

  text += "\n\nВаш ответ: <b>";
  text += EscapeHtml(answer_display);
  text += "</b>";

  if (std::holds_alternative<MatchingTask>(question.task)) {
    if (feedback.result == AnswerResult::kCorrect) {
      text += "\n✅ Все пары без ошибок!";
    } else if (!feedback.completed) {
      const auto attempts_left = 3 - feedback.attempts;
      text += "\n⚠️ Пары собраны, но были ошибки. Карточка вернётся позже — осталось попыток: ";
      text += std::to_string(attempts_left);
      text += '.';
    } else {
      text += "\n❌ Пары собраны с ошибками и не засчитаны после трёх попыток.";
    }
  } else if (feedback.result == AnswerResult::kCorrect) {
    if (feedback.attempts == 1) {
      text += "\n✅ Верно с первой попытки!";
    } else if (feedback.attempts == 2) {
      text += "\n✅ Верно со второй попытки.";
    } else {
      text += "\n✅ Верно с третьей попытки.";
    }
  } else if (!feedback.completed) {
    const auto attempts_left = 3 - feedback.attempts;
    text += "\n❌ Пока неверно. Вопрос вернётся позже — осталось попыток: ";
    text += std::to_string(attempts_left);
    text += '.';
  } else {
    text += "\n❌ Не засчитано после трёх попыток.";
  }

  if (feedback.completed && feedback.previous_topic_level != progress.topic_level) {
    text += feedback.previous_topic_level < progress.topic_level ? "\n⬆️ Уровень темы: " : "\n⬇️ Уровень темы: ";
    text += std::to_string(feedback.previous_topic_level);
    text += " → ";
    text += std::to_string(progress.topic_level);
  }

  if (feedback.completed && feedback.previous_adaptive_level != progress.adaptive_level) {
    text += feedback.previous_adaptive_level < progress.adaptive_level ? "\n⬆️ Общий уровень: " : "\n⬇️ Общий уровень: ";
    text += std::to_string(feedback.previous_adaptive_level);
    text += " → ";
    text += std::to_string(progress.adaptive_level);
  }

  if (!feedback.show_explanation) {
    return text;
  }

  text += "\n\n";
  text += EscapeHtml(question.explanation);

  if (question.source) {
    text += "\n\n<a href=\"";
    text += EscapeHtml(*question.source);
    text += "\">Источник</a>";
  }

  return text;
}
