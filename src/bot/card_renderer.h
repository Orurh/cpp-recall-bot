#pragma once

#include "quiz/question.h"

#include <boost/json/object.hpp>
#include <cstdint>

boost::json::object BuildQuestionMessage(std::int64_t chat_id, const Question& question);