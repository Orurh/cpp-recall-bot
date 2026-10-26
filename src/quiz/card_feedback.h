#pragma once

#include <cstdint>
#include <stdexcept>
#include <string>

struct CardFeedbackSubmission {
  std::int64_t user_id = 0;
  std::int64_t session_id = 0;
  std::string request_id;
  std::string question_id;
  std::string public_id;
  std::string content_revision;
  std::string content_snapshot_json;
  std::string phase;
  std::string category;
  std::string comment;
};

struct CardFeedbackRecord {
  std::int64_t id = 0;
  CardFeedbackSubmission submission;
  std::string status;
  std::string decision;
  std::string source;
  std::int64_t version = 1;
  std::string created_at;
  std::string updated_at;
  std::string reference_context_json = "[]";
};

class FeedbackRateLimitError : public std::runtime_error {
 public:
  using std::runtime_error::runtime_error;
};

class FeedbackAccessDenied : public std::runtime_error {
 public:
  using std::runtime_error::runtime_error;
};
