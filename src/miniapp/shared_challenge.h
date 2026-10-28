#pragma once

#include "quiz/question.h"

#include <algorithm>
#include <array>
#include <string_view>

struct SharedChallengeDefinition {
  std::string_view id;
  std::string_view question_id;
  std::string_view content_revision;
};

// Explicitly reviewed revisions only. A content edit withdraws the link until
// its replacement has been reviewed; a tag is not permission to publish a card.
inline constexpr std::array<SharedChallengeDefinition, 2> kSharedChallenges{{
    {"review_212_v1", "review_cache_read_contract", "db411f6b10d0dea27d6b990051df6a52d2a105eb5a31bc4e1ec5006e0dbca8f6"},
    {"review_213_v1", "review_mark_existing_contract",
     "30c2a18063c0a9f63ba2f3df5b2e346c34871af5e4fd4f0dc7b167e04ec632bc"},
}};

inline const SharedChallengeDefinition* FindSharedChallenge(std::string_view id) {
  const auto found = std::ranges::find(kSharedChallenges, id, &SharedChallengeDefinition::id);
  return found == kSharedChallenges.end() ? nullptr : &*found;
}

inline const SharedChallengeDefinition* SharedChallengeForQuestion(const Question& question) {
  const auto found = std::ranges::find(kSharedChallenges, question.id, &SharedChallengeDefinition::question_id);
  return found == kSharedChallenges.end() || found->content_revision != question.content_revision ? nullptr : &*found;
}

inline bool IsValidBotUsername(std::string_view username) {
  return username.size() >= 5 && username.size() <= 32 && std::ranges::all_of(username, [](char ch) {
           return (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9') || ch == '_';
         });
}
