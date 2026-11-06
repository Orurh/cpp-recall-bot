#pragma once

#include "system_design/repository.h"

#include <stdexcept>

namespace system_design {

class AccessDenied : public std::runtime_error {
 public:
  AccessDenied() : std::runtime_error("Это направление пока доступно только владельцу.") {}
};

class Service {
 public:
  Service(std::shared_ptr<DatabaseConnectionPool> pool, std::int64_t owner_id);
  [[nodiscard]] bool IsOwner(std::int64_t user_id) const noexcept;
  void RequireOwner(std::int64_t user_id) const;
  [[nodiscard]] boost::json::object Handle(std::int64_t user_id, std::string_view action,
                                           const boost::json::object& request);

 private:
  [[nodiscard]] boost::json::object Serialize(const Session& session) const;
  Repository repository_;
  std::int64_t owner_id_;
};

}  // namespace system_design
