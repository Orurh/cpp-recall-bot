#pragma once

#include <boost/json.hpp>

#include <string>
#include <string_view>

class TelegramBot {
 public:
  explicit TelegramBot(std::string token);

  boost::json::value Call(std::string_view method, const boost::json::object& params);

 private:
  std::string token_;
};