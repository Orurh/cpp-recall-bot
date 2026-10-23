#pragma once

#include "learning/progress_summary.h"

#include <boost/json/object.hpp>

#include <cstdint>
#include <string_view>

boost::json::object BuildMiniAppMenuMessage(std::int64_t chat_id, std::string_view mini_app_url,
                                            std::string_view notice = {});

boost::json::object BuildProgressSummaryMessage(std::int64_t chat_id, const UserProgressSummary& summary);
