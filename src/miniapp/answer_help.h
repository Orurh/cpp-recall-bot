#pragma once

#include "quiz/question_identity.h"

#include <boost/json.hpp>

#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>

// Read only the archived revision presented to the learner, never today's key.
inline boost::json::object ParseFeedbackContent(std::string_view snapshot, std::string_view question_id,
                                                std::string_view revision) {
  auto content = boost::json::parse(snapshot).as_object();
  if (revision.size() != 64 || content.at("id").as_string() != question_id ||
      QuestionContentRevision(CanonicalQuestionJson(content)) != revision) {
    throw std::invalid_argument("Редакция разбора недоступна. Открой карточку заново.");
  }
  return content;
}

inline std::optional<boost::json::object> SingleChoiceAnswerHelp(const boost::json::object& content) {
  const auto& task = content.at("task").as_object();
  if (task.at("type").as_string() != "single_choice")
    return std::nullopt;
  const auto& key = task.at("correct_option_id").as_string();
  for (const auto& value : task.at("options").as_array()) {
    const auto& option = value.as_object();
    if (option.at("id").as_string() != key)
      continue;
    boost::json::object result{{"correct_answer", option.at("text")}, {"explanation", ""}};
    if (const auto* explanation = content.if_contains("explanation"); explanation && explanation->is_string())
      result["explanation"] = *explanation;
    if (const auto* source = content.if_contains("source"); source && source->is_string())
      result["source"] = *source;
    return result;
  }
  throw std::invalid_argument("Ответ для этой редакции недоступен.");
}
