#pragma once

#include "miniapp/telegram_init_data.h"
#include "quiz/question_bank.h"
#include "storage/database_connection_pool.h"

#include <boost/json/object.hpp>

#include <cstdint>
#include <memory>
#include <string>

class MiniAppService {
 public:
  MiniAppService(const QuestionBank& questions, std::string database_url);
  MiniAppService(const QuestionBank& questions, std::shared_ptr<DatabaseConnectionPool> connection_pool);
  ~MiniAppService();

  MiniAppService(const MiniAppService&) = delete;
  MiniAppService& operator=(const MiniAppService&) = delete;

  boost::json::object Bootstrap(const TelegramMiniAppUser& user);
  boost::json::object StartSession(std::int64_t user_id, const boost::json::object& request);
  boost::json::object CurrentSession(std::int64_t user_id);
  boost::json::object SubmitAnswer(std::int64_t user_id, const boost::json::object& request);
  boost::json::object SelectMatching(std::int64_t user_id, const boost::json::object& request);
  boost::json::object OpenTheory(std::int64_t user_id, const boost::json::object& request);
  boost::json::object NextQuestion(std::int64_t user_id);
  boost::json::object StopSession(std::int64_t user_id);
  boost::json::object SetProfileFrame(std::int64_t user_id, const boost::json::object& request);
  boost::json::object SaveTrainingPreset(std::int64_t user_id, const boost::json::object& request);

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};
