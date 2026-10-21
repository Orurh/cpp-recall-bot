#include "card_renderer.h"

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

  std::string result(topic);

  std::ranges::replace(result, '_', ' ');

  if (result.front() >= 'a' && result.front() <= 'z') {
    result.front() = static_cast<char>(
        result.front() - 'a' + 'A');
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

  if (progress.topic_level < 1 || progress.topic_level > 10 ||
      progress.adaptive_level < 1 || progress.adaptive_level > 10) {
    throw std::invalid_argument("invalid training level");
  }

  std::string text = "<b>";
  text += EscapeHtml(TopicDisplayName(progress.topic));
  text += " · ур. темы ";
  text += std::to_string(progress.topic_level);
  text += "</b> · ";

  if (progress.repeat) {
    text += "Повтор";
  } else {
    if (progress.question_number == 0 || progress.question_number > progress.session_goal) {
      throw std::invalid_argument("invalid training question number");
    }

    text += "Вопрос ";
    text += std::to_string(progress.question_number);
    text += '/';
    text += std::to_string(progress.session_goal);
  }

  text += "\nОбщий ур. ";
  text += std::to_string(progress.adaptive_level);
  text += " · Прогресс ";
  text += BuildProgressBar(progress.completed_questions, progress.session_goal);
  text += ' ';
  text += std::to_string(progress.completed_questions);
  text += '/';
  text += std::to_string(progress.session_goal);

  text += "\nТочность ";

  if (progress.total_attempts == 0) {
    text += "—";
  } else {
    const auto accuracy =
        ((progress.correct_attempts * 100) +
         (progress.total_attempts / 2)) /
        progress.total_attempts;

    text += std::to_string(accuracy);
    text += '%';
  }

  text += " · Серия ";
  text += std::to_string(progress.current_streak);

  return text;
}

}  // namespace

boost::json::object BuildStartMenuKeyboard(std::span<const TrainingContentMode> selected_modes) {
  boost::json::array keyboard;

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
                                          std::string_view notice) {
  std::string text;

  if (!notice.empty()) {
    text += notice;
    text += "\n\n";
  }

  text +=
      "Что тренируем?\n\n"
      "Выбери один или несколько режимов.\n"
      "Сложность подбирается автоматически.";

  return {
      {"chat_id", chat_id},
      {"text", std::move(text)},
      {"reply_markup", BuildStartMenuKeyboard(selected_modes)},
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
        const auto position = side == MatchingSide::kLeft
                                  ? left_positions[row_index] : right_positions[row_index];
        const auto pair_index = matching->PairIndex(side, position);
        std::string label;
        if (matching->IsSelected(side, pair_index)) {
          label += "🔹 ";
        }
        label += side == MatchingSide::kLeft
                     ? task->pairs[pair_index].left
                     : task->pairs[pair_index].right;
        const std::string data =
            std::string(side == MatchingSide::kLeft ? "match:l:" : "match:r:") +
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
                                          const TrainingProgressView& progress,
                                          const MatchingRound* matching) {
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
                            std::string_view answer_display, AnswerResult result, bool show_explanation,
                            const TrainingProgressView& progress) {
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
    text += result == AnswerResult::kCorrect
                ? "\n✅ Все пары без ошибок!"
                : "\n⚠️ Пары собраны, но были ошибки.";
  } else if (result == AnswerResult::kCorrect) {
    text += "\n✅ Верно!";
  } else {
    text += "\n❌ Неверно.";
  }

  if (!show_explanation) {
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
