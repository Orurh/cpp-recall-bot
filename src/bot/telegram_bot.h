#pragma once

#include "http/curl_http_client.h"

#include <boost/json.hpp>

#include <chrono>
#include <stop_token>
#include <string>
#include <string_view>

class TelegramBot {
 public:
  explicit TelegramBot(std::string token, std::chrono::milliseconds request_timeout = std::chrono::seconds(35));

  boost::json::value Call(std::string_view method, const boost::json::object& params,
                          const std::stop_token& stop_token = {});

 private:
  CurlHttpClient http_;
  std::string token_;
};
