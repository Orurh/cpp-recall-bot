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
  MiniAppService(const QuestionBank& questions, std::string database_url, std::int64_t feedback_admin_id = 0);
  MiniAppService(const QuestionBank& questions, std::shared_ptr<DatabaseConnectionPool> connection_pool,
                 std::int64_t feedback_admin_id = 0);
  ~MiniAppService();

  MiniAppService(const MiniAppService&) = delete;
  MiniAppService& operator=(const MiniAppService&) = delete;

  boost::json::object Bootstrap(const TelegramMiniAppUser& user);
  boost::json::object StartSession(std::int64_t user_id, const boost::json::object& request);
  boost::json::object CurrentSession(std::int64_t user_id);
  boost::json::object ResumeSession(std::int64_t user_id, const boost::json::object& request);
  boost::json::object SubmitAnswer(std::int64_t user_id, const boost::json::object& request);
  boost::json::object SelectMatching(std::int64_t user_id, const boost::json::object& request);
  boost::json::object OpenTheory(std::int64_t user_id, const boost::json::object& request);
  boost::json::object OpenReference(std::int64_t user_id, const boost::json::object& request);
  boost::json::object NextQuestion(std::int64_t user_id, const boost::json::object& request = {});
  boost::json::object SessionResult(std::int64_t user_id, const boost::json::object& request);
  boost::json::object StopSession(std::int64_t user_id, const boost::json::object& request = {});
  boost::json::object SetProfileFrame(std::int64_t user_id, const boost::json::object& request);
  boost::json::object SaveTrainingPreset(std::int64_t user_id, const boost::json::object& request);
  [[nodiscard]] bool IsFeedbackAdmin(std::int64_t user_id) const noexcept;
  boost::json::object SubmitCardFeedback(std::int64_t user_id, const boost::json::object& request);
  boost::json::object ListCardFeedback(std::int64_t user_id, const boost::json::object& request);
  boost::json::object ReviewCardFeedback(std::int64_t user_id, const boost::json::object& request);
  [[nodiscard]] bool CheckReady();
  boost::json::object RecordActivity(std::int64_t user_id, const boost::json::object& request);
  boost::json::object Statistics(std::int64_t user_id, const boost::json::object& request);

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};
