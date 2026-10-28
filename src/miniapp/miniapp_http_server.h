#pragma once

#include "quiz/question_bank.h"
#include "storage/database_connection_pool.h"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <stop_token>
#include <string>

struct MiniAppServerConfig {
  std::string public_url;
  std::string public_path = "/";
  std::string listen_address = "127.0.0.1";
  std::uint16_t port = 8080;
  std::string static_directory = "docs/miniapp";
  std::chrono::seconds auth_max_age = std::chrono::hours(24);
  std::size_t worker_count = 4;
  std::int64_t feedback_admin_id = 0;
  std::string bot_username = "cpp_recall_bot";
};

[[nodiscard]]
std::optional<MiniAppServerConfig> LoadMiniAppServerConfig();

class MiniAppHttpServer {
 public:
  MiniAppHttpServer(MiniAppServerConfig config, std::string bot_token, const QuestionBank& questions,
                    std::shared_ptr<DatabaseConnectionPool> connection_pool);
  ~MiniAppHttpServer();

  MiniAppHttpServer(const MiniAppHttpServer&) = delete;
  MiniAppHttpServer& operator=(const MiniAppHttpServer&) = delete;

  void Run(const std::stop_token& stop_token);

  [[nodiscard]] const MiniAppServerConfig& Config() const noexcept;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};
