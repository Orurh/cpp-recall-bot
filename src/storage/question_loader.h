#pragma once

#include "quiz/question.h"

#include <filesystem>

Question LoadQuestion(const std::filesystem::path& path);
std::vector<Question> LoadQuestions(const std::filesystem::path& directory);