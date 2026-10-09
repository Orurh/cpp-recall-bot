#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>

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

  void SetAdaptiveLevel(
      std::int64_t user_id,
      int level);

  [[nodiscard]]
  std::unordered_map<std::string, int> LoadTopicLevels(
      std::int64_t user_id);

  [[nodiscard]]
  std::int64_t StartSession(
      std::int64_t user_id,
      std::string_view mode);

  void RecordAttempt(
      std::int64_t user_id,
      std::int64_t session_id,
      std::string_view question_id,
      std::string_view answer,
      bool correct,
      std::size_t attempt_number);

  void RecordQuestionCompletion(
      std::int64_t user_id,
      std::string_view question_id,
      std::string_view topic,
      bool correct,
      std::size_t attempts,
      int topic_level);

  void FinishSession(
      std::int64_t session_id,
      std::string_view finish_reason);

  void FinishOpenSessions(
      std::int64_t user_id,
      std::string_view finish_reason);

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};
