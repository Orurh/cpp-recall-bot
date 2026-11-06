#pragma once

#include "storage/database_connection_pool.h"

#include <boost/json.hpp>

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

namespace system_design {

struct Session {
  std::int64_t id = 0;
  std::int64_t version = 1;
  std::string case_id;
  std::string content_revision;
  std::string status = "active";
  std::size_t current_stage = 0;
  boost::json::object state;
  std::optional<boost::json::object> review;
};

class Repository {
 public:
  explicit Repository(std::shared_ptr<DatabaseConnectionPool> pool);
  [[nodiscard]] bool ModuleCompleted(std::int64_t user_id, std::string_view id, std::string_view revision);
  void CompleteModule(std::int64_t user_id, std::string_view id, std::string_view revision);
  [[nodiscard]] Session Start(std::int64_t user_id, std::string_view case_id, std::string_view revision,
                              std::string_view request_id, const boost::json::object& initial_state);
  [[nodiscard]] std::optional<Session> Active(std::int64_t user_id);
  [[nodiscard]] Session Load(std::int64_t user_id, std::int64_t session_id);
  [[nodiscard]] boost::json::array History(std::int64_t user_id);
  [[nodiscard]] boost::json::object Statistics(std::int64_t user_id);
  [[nodiscard]] Session Mutate(std::int64_t user_id, std::int64_t session_id, std::int64_t expected_version,
                               std::string_view request_id, const boost::json::object& request,
                               const std::function<void(Session&)>& operation);

 private:
  std::shared_ptr<DatabaseConnectionPool> pool_;
};

}  // namespace system_design
