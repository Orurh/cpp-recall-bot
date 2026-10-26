#include "storage/postgres_connection.h"

#include <poll.h>

#include <algorithm>
#include <cerrno>
#include <memory>
#include <stdexcept>
#include <utility>

namespace {
using ResultOwner = std::unique_ptr<PGresult, decltype(&PQclear)>;
}

PostgresConnection::PostgresConnection(std::string database_url, DatabaseTimeouts timeouts, std::stop_token stop_token)
    : database_url_(std::move(database_url)), timeouts_(timeouts), stop_token_(stop_token) {
  if (timeouts_.connect.count() <= 0 || timeouts_.statement.count() <= 0 || timeouts_.lock.count() <= 0 ||
      timeouts_.lock >= timeouts_.statement || timeouts_.statement >= timeouts_.query ||
      timeouts_.acquire.count() <= 0 || timeouts_.cleanup.count() <= 0 || timeouts_.migration < timeouts_.query) {
    throw std::invalid_argument("invalid PostgreSQL timeout policy");
  }
  try {
    Connect(std::chrono::steady_clock::now() + timeouts_.connect);
  } catch (...) {
    Close();
    throw;
  }
}

PostgresConnection::~PostgresConnection() {
  Close();
}

void PostgresConnection::Close() noexcept {
  if (connection_ != nullptr) {
    PQfinish(std::exchange(connection_, nullptr));
  }
}

void PostgresConnection::CheckDeadline(Deadline deadline, bool cleanup) const {
  if (!cleanup && stop_token_.stop_requested()) {
    throw std::runtime_error("PostgreSQL operation stopped");
  }
  if (std::chrono::steady_clock::now() >= deadline) {
    throw std::runtime_error("PostgreSQL operation timed out");
  }
}

short PostgresConnection::WaitSocket(short events, Deadline deadline, bool cleanup) const {
  for (;;) {
    CheckDeadline(deadline, cleanup);
    const auto remaining = std::chrono::ceil<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now());
    const auto wait = std::clamp(remaining, std::chrono::milliseconds{1}, std::chrono::milliseconds{25});
    pollfd descriptor{.fd = PQsocket(connection_), .events = events, .revents = 0};
    if (descriptor.fd < 0) {
      throw std::runtime_error("PostgreSQL socket is unavailable");
    }
    const int ready = poll(&descriptor, 1, static_cast<int>(wait.count()));
    if (ready < 0 && errno != EINTR) {
      throw std::runtime_error("PostgreSQL socket polling failed");
    }
    if (ready > 0) {
      return descriptor.revents;
    }
  }
}

void PostgresConnection::Connect(Deadline deadline) {
  Close();
  CheckDeadline(deadline, false);
  const char* keys[] = {"dbname", "fallback_application_name", "tcp_user_timeout", nullptr};
  const char* values[] = {database_url_.c_str(), "cpp-recall-bot", "3000", nullptr};
  connection_ = PQconnectStartParams(keys, values, 1);
  if (connection_ == nullptr) {
    throw std::runtime_error("failed to allocate PostgreSQL connection");
  }
  auto status = PGRES_POLLING_WRITING;
  while (status != PGRES_POLLING_OK) {
    CheckDeadline(deadline, false);
    if (status == PGRES_POLLING_FAILED) {
      throw std::runtime_error("failed to connect to PostgreSQL");
    }
    if (status == PGRES_POLLING_READING || status == PGRES_POLLING_WRITING) {
      static_cast<void>(WaitSocket(status == PGRES_POLLING_READING ? POLLIN : POLLOUT, deadline, false));
    }
    status = PQconnectPoll(connection_);
  }
  if (PQsetnonblocking(connection_, 1) != 0) {
    throw std::runtime_error("failed to set nonblocking PostgreSQL connection");
  }
  const std::vector<std::string> settings{
      std::to_string((migration_ ? timeouts_.migration : timeouts_.statement).count()),
      std::to_string((migration_ ? timeouts_.migration : timeouts_.lock).count())};
  ResultOwner configured(Exchange("SELECT set_config('statement_timeout', $1, false), "
                                  "set_config('lock_timeout', $2, false);",
                                  &settings, deadline),
                         PQclear);
  if (PQresultStatus(configured.get()) != PGRES_TUPLES_OK) {
    throw std::runtime_error("failed to configure PostgreSQL timeout policy");
  }
}

PGresult* PostgresConnection::Exchange(const char* sql, const std::vector<std::string>* params, Deadline deadline,
                                       bool cleanup) {
  CheckDeadline(deadline, cleanup);
  std::vector<const char*> values;
  if (params != nullptr) {
    for (const auto& value : *params) {
      values.push_back(value.c_str());
    }
  }
  const int sent = params == nullptr ? PQsendQuery(connection_, sql)
                                     : PQsendQueryParams(connection_, sql, static_cast<int>(values.size()), nullptr,
                                                         values.data(), nullptr, nullptr, 0);
  if (sent != 1) {
    throw std::runtime_error("failed to send PostgreSQL query");
  }
  for (;;) {
    CheckDeadline(deadline, cleanup);
    const int flushed = PQflush(connection_);
    if (flushed == 0) {
      break;
    }
    if (flushed < 0) {
      throw std::runtime_error("failed to flush PostgreSQL query");
    }
    const auto events = WaitSocket(POLLIN | POLLOUT, deadline, cleanup);
    if ((events & (POLLIN | POLLHUP | POLLERR)) != 0 && PQconsumeInput(connection_) != 1) {
      throw std::runtime_error("failed to read PostgreSQL response");
    }
  }
  ResultOwner result(nullptr, PQclear);
  for (;;) {
    CheckDeadline(deadline, cleanup);
    while (PQisBusy(connection_) != 0) {
      static_cast<void>(WaitSocket(POLLIN, deadline, cleanup));
      if (PQconsumeInput(connection_) != 1) {
        throw std::runtime_error("failed to read PostgreSQL response");
      }
    }
    ResultOwner next(PQgetResult(connection_), PQclear);
    if (!next) {
      break;
    }
    const auto status = PQresultStatus(next.get());
    if (status == PGRES_COPY_IN || status == PGRES_COPY_OUT || status == PGRES_COPY_BOTH) {
      throw std::runtime_error("COPY is not supported by the application connection");
    }
    if (!result || PQresultStatus(result.get()) != PGRES_FATAL_ERROR) {
      result = std::move(next);
    }
  }
  if (!result) {
    throw std::runtime_error("PostgreSQL returned no result");
  }
  error_ = PQresultErrorMessage(result.get());
  return result.release();
}

PGresult* PostgresConnection::Execute(const char* sql, const std::vector<std::string>* params, bool allow_reconnect,
                                      std::chrono::milliseconds budget) {
  const auto deadline = std::chrono::steady_clock::now() + (budget.count() > 0 ? budget
                                                            : migration_       ? timeouts_.migration
                                                                               : timeouts_.query);
  try {
    CheckDeadline(deadline, false);
    if (connection_ == nullptr || PQstatus(connection_) != CONNECTION_OK) {
      if (!allow_reconnect) {
        throw std::runtime_error("PostgreSQL connection was lost during a transaction");
      }
      Connect(std::min(deadline, std::chrono::steady_clock::now() + timeouts_.connect));
    }
    return Exchange(sql, params, deadline);
  } catch (...) {
    Close();
    throw;
  }
}

const std::string& PostgresConnection::ErrorMessage() const noexcept {
  return error_;
}

void PostgresConnection::Rollback() noexcept {
  if (connection_ == nullptr || PQstatus(connection_) != CONNECTION_OK) {
    return;
  }
  if (stop_token_.stop_requested()) {
    Close();
    return;
  }
  try {
    ResultOwner result(Exchange("ROLLBACK", nullptr, std::chrono::steady_clock::now() + timeouts_.cleanup, true),
                       PQclear);
    if (PQresultStatus(result.get()) != PGRES_COMMAND_OK) {
      Close();
    }
  } catch (...) {
    Close();
  }
}

void PostgresConnection::SetMigrationMode(bool enabled) {
  migration_ = enabled;
  const std::vector<std::string> settings{std::to_string((enabled ? timeouts_.migration : timeouts_.statement).count()),
                                          std::to_string((enabled ? timeouts_.migration : timeouts_.lock).count())};
  ResultOwner configured(Execute("SELECT set_config('statement_timeout', $1, false), "
                                 "set_config('lock_timeout', $2, false);",
                                 &settings, false),
                         PQclear);
  if (PQresultStatus(configured.get()) != PGRES_TUPLES_OK) {
    Close();
    throw std::runtime_error("failed to change PostgreSQL timeout policy");
  }
}
