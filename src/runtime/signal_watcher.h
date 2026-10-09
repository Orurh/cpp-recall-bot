// signal_watcher.h
#pragma once

#include <boost/asio/io_context.hpp>
#include <boost/asio/signal_set.hpp>

#include <stop_token>
#include <thread>

class SignalWatcher {
 public:
  SignalWatcher();
  ~SignalWatcher();

  SignalWatcher(const SignalWatcher&) = delete;
  SignalWatcher& operator=(const SignalWatcher&) = delete;

  void Start();

  [[nodiscard]]
  std::stop_token GetStopToken() const noexcept;

 private:
  boost::asio::io_context io_;
  boost::asio::signal_set signals_;
  std::stop_source stop_source_;
  std::jthread worker_;
};