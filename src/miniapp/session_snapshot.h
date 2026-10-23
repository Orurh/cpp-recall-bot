#pragma once

#include "app/training_mode.h"
#include "quiz/matching_round.h"
#include "quiz/question_bank.h"
#include "quiz/session.h"

#include <boost/json.hpp>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

inline constexpr int kMiniAppSessionSnapshotSchemaVersion = 1;
inline constexpr std::string_view kMiniAppSessionContentVersion = "cpp-recall-session-v1";

struct MiniAppSessionState {
  QuizSession quiz;
  std::int64_t storage_session_id = 0;
  std::int64_t snapshot_version = 0;
  std::vector<TrainingContentMode> modes;
  std::vector<std::string> topics;
  int selected_level = 1;
  bool tracks_level_progress = false;
  std::vector<std::string> option_order;
  std::optional<MatchingRound> matching;
  std::size_t questions_started = 0;
  std::size_t current_attempt = 1;
  std::int64_t xp_gained = 0;
  std::optional<std::string> last_question_id;
  std::optional<boost::json::object> last_feedback;
};

[[nodiscard]] std::string SerializeMiniAppSessionSnapshot(const MiniAppSessionState& session);

[[nodiscard]] MiniAppSessionState ParseMiniAppSessionSnapshot(std::string_view json, std::int64_t storage_session_id,
                                                              std::int64_t snapshot_version,
                                                              const QuestionBank& questions);
