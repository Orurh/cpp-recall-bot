// signal_watcher.cc
#include "signal_watcher.h"

#include <spdlog/spdlog.h>

#include <boost/system/error_code.hpp>

#include <csignal>
#include <stdexcept>

SignalWatcher::SignalWatcher() : signals_(io_, SIGINT, SIGTERM) {}

SignalWatcher::~SignalWatcher() {
  boost::system::error_code error;

  [[maybe_unused]] const auto result = signals_.cancel(error);

  if (error) {
    spdlog::warn("failed to cancel signal watcher: {}", error.message());
  }

  io_.stop();
}

void SignalWatcher::Start() {
  if (worker_.joinable()) {
    throw std::logic_error("SignalWatcher already started");
  }

  signals_.async_wait([this](const boost::system::error_code& error, int signal_number) {
    if (error) {
      return;
    }

    const bool stop_requested = stop_source_.request_stop();

    if (stop_requested) {
      spdlog::info("shutdown requested, signal={}", signal_number);
    }

    io_.stop();
  });

  worker_ = std::jthread([this] { io_.run(); });
}

std::stop_token SignalWatcher::GetStopToken() const noexcept {
  return stop_source_.get_token();
}