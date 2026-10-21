#pragma once

#include "quiz/matching_round.h"
#include "quiz/session.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

struct TelegramSession {
  std::int64_t user_id = 0;
  QuizSession quiz;
  std::int64_t storage_session_id = 0;
  std::optional<std::int64_t> message_id;
  std::size_t questions_started = 0;
  bool current_question_is_repeat = false;
  std::vector<std::string> option_order;
  std::vector<std::string> selected_option_ids;
  std::optional<MatchingRound> matching;
  std::optional<std::string> pending_explanation_question_id;
  std::optional<std::string> pending_explanation_answer_text;
};
