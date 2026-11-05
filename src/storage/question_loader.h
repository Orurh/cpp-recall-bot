#pragma once

#include "quiz/question.h"

#include <filesystem>
#include <string_view>

Question ParseQuestionContent(std::string_view content);
Question LoadQuestion(const std::filesystem::path& path);
std::vector<Question> LoadQuestions(const std::filesystem::path& directory);
