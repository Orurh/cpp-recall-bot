#pragma once

#include <cstdint>
#include <string>
#include <vector>

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
  std::int64_t questions_seen = 0;
  std::int64_t completed_count = 0;
  std::int64_t correct_count = 0;
  std::int64_t failed_count = 0;
  std::int64_t first_try_correct_count = 0;
};

struct UserProgressSummary {
  int adaptive_level = 3;
  int adaptive_evidence = 0;
  std::int64_t sessions_started = 0;
  std::int64_t sessions_completed = 0;
  std::int64_t questions_seen = 0;
  std::int64_t completed_count = 0;
  std::int64_t correct_count = 0;
  std::int64_t failed_count = 0;
  std::int64_t first_try_correct_count = 0;
  std::int64_t due_review_count = 0;
  std::vector<TopicProgressSummary> topics;
  std::vector<SkillProgressSummary> skills;
};
