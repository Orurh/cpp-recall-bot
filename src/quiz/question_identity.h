#pragma once

#include <boost/json/value.hpp>

#include <string>
#include <string_view>

// Object field order and formatting do not change a content revision. Array order does.
[[nodiscard]] std::string CanonicalQuestionJson(const boost::json::value& content);
[[nodiscard]] std::string QuestionContentRevision(std::string_view canonical_json);
[[nodiscard]] bool IsPublicQuestionId(std::string_view id);
