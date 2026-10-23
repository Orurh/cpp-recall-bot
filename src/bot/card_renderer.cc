#include "card_renderer.h"

#include "quiz/topic_catalog.h"

#include <boost/json.hpp>

#include <algorithm>
#include <stdexcept>
#include <string>
#include <string_view>

namespace {

std::string EscapeHtml(std::string_view text) {
  std::string result;
  result.reserve(text.size());

  for (const char character : text) {
    switch (character) {
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
        result += character;
        break;
    }
  }

  return result;
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

boost::json::object BuildMiniAppMenuMessage(std::int64_t chat_id, std::string_view mini_app_url,
                                            std::string_view notice) {
  std::string text;
  if (!notice.empty()) {
    text += notice;
    text += "\n\n";
  }

  text += "🧠 Тренировки C++ Recall проходят в Mini App.";

  boost::json::object message{
      {"chat_id", chat_id},
      {"text", std::move(text)},
  };

  if (mini_app_url.empty()) {
    message["text"].as_string() += "\n\nMini App сейчас недоступен. Попробуй позже.";
    return message;
  }

  boost::json::array row;
  row.emplace_back(boost::json::object{
      {"text", "Открыть C++ Recall"},
      {"web_app", boost::json::object{{"url", mini_app_url}}},
  });

  boost::json::array keyboard;
  keyboard.emplace_back(std::move(row));
  message["reply_markup"] = boost::json::object{{"inline_keyboard", std::move(keyboard)}};
  return message;
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
  text += "\nС первой попытки: ";
  text += std::to_string(summary.first_try_correct_count);
  text += "\nК повторению сейчас: ";
  text += std::to_string(summary.due_review_count);

  if (summary.completed_count > 0) {
    text += "\nДоля с первой попытки: ";
    text += std::to_string(Percentage(summary.first_try_correct_count, summary.completed_count));
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
      text += std::to_string(topic.first_try_correct_count);
      text += '/';
      text += std::to_string(topic.completed_count);

      if (topic.completed_count > 0) {
        text += " (";
        text += std::to_string(Percentage(topic.first_try_correct_count, topic.completed_count));
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
