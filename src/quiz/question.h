#pragma once

#include <optional>
#include <string>
#include <vector>

struct AnswerOption {
  std::string id;
  std::string text;
};

struct Question {
  std::string id;
  std::string topic;
  int level = 1;
  std::string standard;
  std::string text;
  std::optional<std::string> code;
  std::vector<AnswerOption> options;
  std::string correct_option_id;
  std::string explanation;
  std::optional<std::string> source;
};