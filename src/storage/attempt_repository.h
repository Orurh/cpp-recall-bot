#pragma once

#include "learning/progress_summary.h"
#include "learning/review_state.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>

struct SubmissionRecord {
  std::int64_t user_id = 0;
  std::int64_t session_id = 0;
  std::string_view question_id;
  std::string_view topic;
  std::string_view answer;
  bool correct = false;
  bool completed = false;
  std::size_t attempt_number = 0;
  int topic_level = 3;
  int adaptive_level = 3;
};

enum class RecordSubmissionResult : std::uint8_t {
  kInserted,
  kDuplicate,
};

class AttemptRepository {
 public:
  explicit AttemptRepository(std::string database_url);
  ~AttemptRepository();

  AttemptRepository(const AttemptRepository&) = delete;
  AttemptRepository& operator=(const AttemptRepository&) = delete;

  AttemptRepository(AttemptRepository&&) noexcept;
  AttemptRepository& operator=(AttemptRepository&&) noexcept;

  void EnsureUser(std::int64_t user_id);

  [[nodiscard]]
  int GetAdaptiveLevel(std::int64_t user_id);

  [[nodiscard]]
  std::unordered_map<std::string, int> LoadTopicLevels(std::int64_t user_id);

  [[nodiscard]]
  std::unordered_map<std::string, QuestionReviewState> LoadReviewStates(std::int64_t user_id);

  [[nodiscard]]
  UserProgressSummary LoadProgressSummary(std::int64_t user_id);

  [[nodiscard]]
  std::int64_t StartSession(std::int64_t user_id, std::string_view mode);

  [[nodiscard]]
  RecordSubmissionResult RecordSubmission(const SubmissionRecord& submission);

  void FinishSession(std::int64_t session_id, std::string_view finish_reason);

  void FinishOpenSessions(std::int64_t user_id, std::string_view finish_reason);

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};
