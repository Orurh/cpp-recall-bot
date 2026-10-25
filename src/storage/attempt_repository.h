#pragma once

#include "learning/adaptive_level.h"
#include "learning/progress_summary.h"
#include "learning/review_state.h"
#include "quiz/card_feedback.h"
#include "quiz/question.h"
#include "storage/database_connection_pool.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

struct SubmissionRecord {
  std::int64_t user_id = 0;
  std::int64_t session_id = 0;
  std::string_view question_id;
  std::string_view topic;
  std::string_view skill_id;
  std::string_view question_family_id;
  QuestionEvidenceType evidence_type = QuestionEvidenceType::kRecognition;
  bool transfer = false;
  std::string_view answer;
  bool correct = false;
  bool completed = false;
  bool assisted = false;
  std::size_t attempt_number = 0;
  int topic_level = 3;
  int topic_level_evidence = 0;
  int adaptive_level = 3;
  int adaptive_level_evidence = 0;
  std::string_view content_revision = {};
};

enum class RecordSubmissionResult : std::uint8_t {
  kInserted,
  kDuplicate,
};

struct SkillMasteryChange {
  std::string skill_id;
  SkillMasteryStatus previous = SkillMasteryStatus::kUnassessed;
  SkillMasteryStatus current = SkillMasteryStatus::kUnassessed;
  std::int64_t successful_family_count = 0;
  std::int64_t clean_family_count = 0;
  std::int64_t application_family_count = 0;
  std::int64_t transfer_family_count = 0;
};

struct RecordSubmissionOutcome {
  RecordSubmissionResult result = RecordSubmissionResult::kInserted;
  std::optional<SkillMasteryChange> skill_mastery;
  std::int64_t xp_gained = 0;
  std::vector<std::string> rewards_unlocked;
};

struct ActiveTrainingSessionRecord {
  std::int64_t session_id = 0;
  std::int64_t state_version = 0;
  std::string content_version;
  std::string state_json;
};

struct ActiveSessionUpdate {
  std::int64_t expected_version = 0;
  std::string_view content_version;
  std::function<std::string(const RecordSubmissionOutcome&)> build_state_json;
};

struct TrainingPreset {
  int slot = 1;
  std::string name;
  std::vector<std::string> modes;
  std::vector<std::string> topics;
  int level = 1;
};

struct RewardAccount {
  std::unordered_set<std::string> unlocked_reward_ids;
  std::vector<std::string> newly_unlocked_reward_ids;
  std::string profile_frame = "none";
  int preset_slot_count = 1;
  std::vector<TrainingPreset> presets;
};

class AttemptRepository {
 public:
  explicit AttemptRepository(std::string database_url);
  explicit AttemptRepository(std::shared_ptr<DatabaseConnectionPool> connection_pool);
  ~AttemptRepository();

  AttemptRepository(const AttemptRepository&) = delete;
  AttemptRepository& operator=(const AttemptRepository&) = delete;

  AttemptRepository(AttemptRepository&&) noexcept;
  AttemptRepository& operator=(AttemptRepository&&) noexcept;

  void EnsureUser(std::int64_t user_id);
  [[nodiscard]] bool CheckReady();
  void RecordBotStart(std::int64_t user_id, std::int64_t message_id);
  [[nodiscard]] bool RecordActivity(std::int64_t user_id, std::string_view visit_id, std::int64_t sequence,
                                    std::int64_t session_id, std::string_view question_id, std::string_view context,
                                    std::string_view practice_kind);
  [[nodiscard]] std::string BetaStatistics(int days);
  [[nodiscard]] std::optional<std::int64_t> FindSessionByRequest(std::int64_t user_id, std::string_view request_id);

  [[nodiscard]]
  int GetAdaptiveLevel(std::int64_t user_id);

  [[nodiscard]]
  AdaptiveLevelState GetAdaptiveState(std::int64_t user_id);

  [[nodiscard]]
  int GetUnlockedLevel(std::int64_t user_id);

  [[nodiscard]]
  std::unordered_map<std::string, int> LoadTopicLevels(std::int64_t user_id);

  [[nodiscard]]
  std::unordered_map<std::string, AdaptiveLevelState> LoadTopicStates(std::int64_t user_id);

  [[nodiscard]]
  std::unordered_map<std::string, QuestionReviewState> LoadReviewStates(std::int64_t user_id);

  [[nodiscard]]
  std::unordered_set<std::string> LoadCleanQuestionFamilies(std::int64_t user_id);

  [[nodiscard]]
  UserProgressSummary LoadProgressSummary(std::int64_t user_id);

  [[nodiscard]]
  RewardAccount LoadRewardAccount(std::int64_t user_id);

  void SetProfileFrame(std::int64_t user_id, std::string_view frame_id);

  void SaveTrainingPreset(std::int64_t user_id, const TrainingPreset& preset);

  void ArchiveQuestionContent(const std::vector<const Question*>& questions);
  [[nodiscard]] std::optional<std::string> LoadQuestionContent(std::string_view question_id, std::string_view revision);
  [[nodiscard]] std::optional<CardFeedbackRecord> FindCardFeedback(std::int64_t user_id, std::string_view request_id);
  [[nodiscard]] CardFeedbackRecord SaveCardFeedback(const CardFeedbackSubmission& submission);
  [[nodiscard]] std::vector<CardFeedbackRecord> ListCardFeedback(std::string_view status, std::int64_t before_id);
  [[nodiscard]] CardFeedbackRecord ReviewCardFeedback(std::int64_t feedback_id, std::int64_t expected_version,
                                                      std::int64_t reviewer_id, std::string_view status,
                                                      std::string_view decision, std::string_view source);

  [[nodiscard]]
  std::int64_t StartSession(std::int64_t user_id, std::string_view mode);

  [[nodiscard]]
  std::optional<ActiveTrainingSessionRecord> LoadActiveTrainingSession(std::int64_t user_id);

  [[nodiscard]]
  std::int64_t StartActiveTrainingSession(std::int64_t user_id, std::string_view mode, std::string_view content_version,
                                          std::string_view state_json, std::string_view request_id = {});

  [[nodiscard]]
  std::int64_t UpdateActiveTrainingSession(std::int64_t user_id, std::int64_t session_id, std::int64_t expected_version,
                                           std::string_view content_version, std::string_view state_json);

  [[nodiscard]]
  RecordSubmissionOutcome RecordSubmission(const SubmissionRecord& submission);

  [[nodiscard]]
  RecordSubmissionOutcome RecordSubmission(const SubmissionRecord& submission,
                                           const ActiveSessionUpdate& active_session_update);

  void FinishSession(std::int64_t session_id, std::string_view finish_reason);

  void FinishOpenSessions(std::int64_t user_id, std::string_view finish_reason);

  void FinishOpenSessionsWithModePrefix(std::int64_t user_id, std::string_view mode_prefix,
                                        std::string_view finish_reason);

 private:
  [[nodiscard]]
  RecordSubmissionOutcome RecordSubmissionImpl(const SubmissionRecord& submission,
                                               const ActiveSessionUpdate* active_session_update);

  struct Impl;
  std::unique_ptr<Impl> impl_;
};
