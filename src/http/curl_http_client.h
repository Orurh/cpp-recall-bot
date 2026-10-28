#pragma once

#include <curl/curl.h>

#include <chrono>
#include <cstdint>
#include <memory>
#include <stop_token>
#include <string>
#include <string_view>

class CurlHttpClient {
 public:
  struct Response {
    std::int64_t status_code = 0;
    std::string body;
  };

  explicit CurlHttpClient(std::chrono::milliseconds request_timeout = std::chrono::seconds(35));

  [[nodiscard]]
  Response PostJson(std::string_view url, std::string_view body, const std::stop_token& stop_token);

 private:
  struct CurlDeleter {
    void operator()(CURL* handle) const noexcept;
  };

  std::unique_ptr<CURL, CurlDeleter> handle_;
  std::chrono::milliseconds request_timeout_;
};
