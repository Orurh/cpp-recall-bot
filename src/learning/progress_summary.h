#pragma once

#include "learning/skill_mastery.h"

#include <cstdint>
#include <string>
#include <vector>

struct AnswerStatistics {
  std::int64_t attempts = 0;
  std::int64_t correct_attempts = 0;
  std::int64_t first_answers = 0;
  std::int64_t correct_first_answers = 0;

  AnswerStatistics& operator+=(const AnswerStatistics& other) {
    attempts += other.attempts;
    correct_attempts += other.correct_attempts;
    first_answers += other.first_answers;
    correct_first_answers += other.correct_first_answers;
    return *this;
  }
};

struct QuestionAnswerStatistics {
  std::string question_id;
  AnswerStatistics answers;
};

struct TopicProgressSummary {
  std::string topic;
  int adaptive_level = 3;
  int adaptive_evidence = 0;
  std::int64_t completed_count = 0;
  std::int64_t correct_count = 0;
  std::int64_t failed_count = 0;
  std::int64_t first_try_correct_count = 0;
};

struct SkillProgressSummary {
  std::string skill_id;
  SkillMasteryStatus mastery_status = SkillMasteryStatus::kUnassessed;
  std::int64_t successful_family_count = 0;
  std::int64_t clean_family_count = 0;
  std::int64_t application_family_count = 0;
  std::int64_t transfer_family_count = 0;
  std::int64_t questions_seen = 0;
  std::int64_t completed_count = 0;
  std::int64_t correct_count = 0;
  std::int64_t failed_count = 0;
  std::int64_t first_try_correct_count = 0;
};

struct UserProgressSummary {
  int adaptive_level = 3;
  int adaptive_evidence = 0;
  int unlocked_level = 3;
  std::int64_t sessions_started = 0;
  std::int64_t sessions_completed = 0;
  std::int64_t questions_seen = 0;
  std::int64_t completed_count = 0;
  std::int64_t correct_count = 0;
  std::int64_t failed_count = 0;
  std::int64_t first_try_correct_count = 0;
  std::int64_t due_review_count = 0;
  std::int64_t xp_total = 0;
  std::vector<TopicProgressSummary> topics;
  std::vector<SkillProgressSummary> skills;
  AnswerStatistics answers = {};
  std::vector<QuestionAnswerStatistics> question_answers = {};
};
