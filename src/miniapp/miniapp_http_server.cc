#include "miniapp/miniapp_http_server.h"

#include "miniapp/miniapp_service.h"
#include "miniapp/shared_challenge.h"
#include "miniapp/telegram_init_data.h"
#include "monitoring/runtime_metrics.h"
#include "quiz/card_feedback.h"
#include "runtime/bounded_executor.h"

#include <boost/asio.hpp>
#include <boost/beast.hpp>
#include <boost/json.hpp>

#include <poll.h>
#include <spdlog/spdlog.h>

#include <cerrno>
#include <charconv>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <mutex>
#include <new>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

namespace {

std::string_view MetricRoute(std::string_view target) {
  constexpr std::string_view routes[] = {"/health/live",
                                         "/health/ready",
                                         "/api/v1/bootstrap",
                                         "/api/v1/sessions",
                                         "/api/v1/session",
                                         "/api/v1/session/answer",
                                         "/api/v1/session/share",
                                         "/api/v1/session/match",
                                         "/api/v1/session/theory",
                                         "/api/v1/session/next",
                                         "/api/v1/session/result",
                                         "/api/v1/session/stop",
                                         "/api/v1/activity",
                                         "/api/v1/admin/statistics",
                                         "/api/v1/rewards/profile-frame",
                                         "/api/v1/training-presets",
                                         "/api/v1/card-feedback",
                                         "/api/v1/admin/card-feedback",
                                         "/api/v1/admin/card-feedback/review",
                                         "/",
                                         "/index.html",
                                         "/app.js",
                                         "/style.css"};
  for (const auto route : routes) {
    if (route == target)
      return route;
  }
  return "other";
}

namespace asio = boost::asio;
namespace beast = boost::beast;
namespace http = beast::http;
using tcp = asio::ip::tcp;

constexpr std::size_t kMaxRequestBody = 64 * 1024;

struct ConnectionWorkers {
  std::shared_ptr<BoundedExecutor> executor;
  std::stop_source shutdown;
  ~ConnectionWorkers() {
    shutdown.request_stop();
    executor->Stop();
  }
};

// Beast expires_after only applies to asynchronous operations. Resume the parser/serializer
// on a non-blocking socket, with an absolute deadline and bounded shutdown latency.
template <typename Operation>
void SocketOperation(tcp::socket& socket, short events, const std::stop_token& stop_token, Operation&& operation,
                     boost::system::error_code& error) {
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
  while (!stop_token.stop_requested() && std::chrono::steady_clock::now() < deadline) {
    operation(error);
    if (error != asio::error::would_block && error != asio::error::try_again)
      return;
    pollfd descriptor{.fd = socket.native_handle(), .events = events, .revents = 0};
    if (poll(&descriptor, 1, 25) < 0 && errno != EINTR) {
      error.assign(errno, boost::system::generic_category());
      return;
    }
  }
  error = stop_token.stop_requested() ? asio::error::operation_aborted : asio::error::timed_out;
}

std::optional<std::string> Environment(std::string_view name) {
  const std::string key(name);
  const char* value = std::getenv(key.c_str());
  if (value == nullptr || *value == '\0')
    return std::nullopt;
  return std::string(value);
}

std::uint16_t ParsePort(std::string_view value) {
  unsigned int port = 0;
  const auto [end, error] = std::from_chars(value.data(), value.data() + value.size(), port);
  if (error != std::errc{} || end != value.data() + value.size() || port == 0 || port > 65535) {
    throw std::invalid_argument("MINI_APP_PORT must be between 1 and 65535");
  }
  return static_cast<std::uint16_t>(port);
}

std::chrono::seconds ParseMaxAge(std::string_view value) {
  std::int64_t seconds = 0;
  const auto [end, error] = std::from_chars(value.data(), value.data() + value.size(), seconds);
  if (error != std::errc{} || end != value.data() + value.size() || seconds < 60 || seconds > 7 * 24 * 60 * 60) {
    throw std::invalid_argument("MINI_APP_AUTH_MAX_AGE_SECONDS must be between 60 and 604800");
  }
  return std::chrono::seconds(seconds);
}

std::size_t ParseWorkerCount(std::string_view value) {
  std::size_t worker_count = 0;
  const auto [end, error] = std::from_chars(value.data(), value.data() + value.size(), worker_count);
  if (error != std::errc{} || end != value.data() + value.size() || worker_count < 2 || worker_count > 32) {
    throw std::invalid_argument("MINI_APP_WORKER_COUNT must be between 2 and 32");
  }
  return worker_count;
}

std::string ExtractPublicPath(std::string_view url) {
  constexpr std::string_view scheme = "https://";
  if (!url.starts_with(scheme)) {
    throw std::invalid_argument("MINI_APP_URL must use HTTPS");
  }
  if (url.find_first_of("?#\r\n\t ") != std::string_view::npos) {
    throw std::invalid_argument("MINI_APP_URL must not contain query or fragment");
  }
  const auto path_start = url.find('/', scheme.size());
  const auto authority = url.substr(
      scheme.size(), path_start == std::string_view::npos ? std::string_view::npos : path_start - scheme.size());
  if (authority.empty()) {
    throw std::invalid_argument("MINI_APP_URL must contain a host");
  }
  std::string path = path_start == std::string_view::npos ? "/" : std::string(url.substr(path_start));
  if (path.back() != '/')
    path.push_back('/');
  return path;
}

std::string ReadTextFile(const std::filesystem::path& path) {
  std::ifstream input(path, std::ios::binary);
  if (!input)
    throw std::runtime_error("cannot open static asset");
  return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

std::string ContentType(std::string_view path) {
  if (path.ends_with(".html"))
    return "text/html; charset=utf-8";
  if (path.ends_with(".js"))
    return "text/javascript; charset=utf-8";
  if (path.ends_with(".css"))
    return "text/css; charset=utf-8";
  if (path.ends_with(".json"))
    return "application/json; charset=utf-8";
  return "application/octet-stream";
}

http::response<http::string_body> MakeResponse(http::status status, std::string body, std::string_view content_type,
                                               unsigned int version) {
  http::response<http::string_body> response{status, version};
  response.set(http::field::server, "cpp-recall-miniapp");
  response.set(http::field::content_type, content_type);
  response.set(http::field::cache_control, "no-store");
  response.set("X-Content-Type-Options", "nosniff");
  response.set("Referrer-Policy", "no-referrer");
  response.set("Content-Security-Policy",
               "default-src 'self'; script-src 'self' https://telegram.org; "
               "style-src 'self' 'unsafe-inline'; connect-src 'self'; "
               "img-src 'self' data: https:; font-src 'self'; object-src 'none'; "
               "base-uri 'none'; form-action 'self'");
  response.keep_alive(false);
  response.body() = std::move(body);
  response.prepare_payload();
  return response;
}

http::response<http::string_body> JsonResponse(http::status status, boost::json::value value, unsigned int version) {
  return MakeResponse(status, boost::json::serialize(value), "application/json; charset=utf-8", version);
}

http::response<http::string_body> ErrorResponse(http::status status, std::string_view code, std::string_view message,
                                                unsigned int version) {
  return JsonResponse(status, boost::json::object{{"error", std::string(code)}, {"message", std::string(message)}},
                      version);
}

std::optional<std::string> RelativeTarget(std::string_view raw_target, std::string_view public_path) {
  const auto query = raw_target.find('?');
  const auto target = raw_target.substr(0, query);
  if (public_path == "/")
    return std::string(target);

  const auto without_slash = public_path.substr(0, public_path.size() - 1);
  if (target == without_slash)
    return std::string{};
  if (!target.starts_with(public_path))
    return std::nullopt;
  std::string relative = "/";
  relative.append(target.substr(public_path.size()));
  return relative;
}

boost::json::object ParseObjectBody(const http::request<http::string_body>& request) {
  if (request.body().empty())
    return {};
  boost::system::error_code error;
  auto value = boost::json::parse(request.body(), error);
  if (error || !value.is_object()) {
    throw std::invalid_argument("request body must be a JSON object");
  }
  return value.as_object();
}

}  // namespace

std::optional<MiniAppServerConfig> LoadMiniAppServerConfig() {
  const auto public_url = Environment("MINI_APP_URL");
  if (!public_url)
    return std::nullopt;

  MiniAppServerConfig config;
  config.public_url = *public_url;
  config.public_path = ExtractPublicPath(*public_url);
  if (config.public_url.back() != '/') {
    config.public_url.push_back('/');
  }
  if (const auto value = Environment("MINI_APP_LISTEN_ADDRESS")) {
    config.listen_address = *value;
  }
  if (const auto value = Environment("MINI_APP_PORT")) {
    config.port = ParsePort(*value);
  }
  if (const auto value = Environment("MINI_APP_STATIC_DIR")) {
    config.static_directory = *value;
  }
  if (const auto value = Environment("MINI_APP_BOT_USERNAME")) {
    if (*value != "off" && !IsValidBotUsername(*value))
      throw std::invalid_argument("MINI_APP_BOT_USERNAME must be a bot username without @ or URL");
    config.bot_username = *value == "off" ? std::string{} : *value;
  }
  if (const auto value = Environment("MINI_APP_AUTH_MAX_AGE_SECONDS")) {
    config.auth_max_age = ParseMaxAge(*value);
  }
  if (const auto value = Environment("MINI_APP_WORKER_COUNT")) {
    config.worker_count = ParseWorkerCount(*value);
  }
  if (const auto value = Environment("MINI_APP_FEEDBACK_ADMIN_ID")) {
    const auto [end, error] = std::from_chars(value->data(), value->data() + value->size(), config.feedback_admin_id);
    if (error != std::errc{} || end != value->data() + value->size() || config.feedback_admin_id <= 0) {
      throw std::invalid_argument("MINI_APP_FEEDBACK_ADMIN_ID must be a positive Telegram user ID");
    }
  }
  return config;
}

struct MiniAppHttpServer::Impl {
  Impl(MiniAppServerConfig server_config, std::string token, const QuestionBank& questions,
       std::shared_ptr<DatabaseConnectionPool> connection_pool)
      : config(std::move(server_config)),
        bot_token(std::move(token)),
        pool(std::move(connection_pool)),
        service(questions, pool, config.feedback_admin_id, config.bot_username) {}

  MiniAppServerConfig config;
  std::string bot_token;
  std::shared_ptr<DatabaseConnectionPool> pool;
  MiniAppService service;
  std::shared_ptr<BoundedExecutor> connections;

  std::optional<TelegramInitData> Authenticate(const http::request<http::string_body>& request) const {
    const auto authorization = request[http::field::authorization];
    constexpr std::string_view prefix = "tma ";
    const std::string_view value(authorization.data(), authorization.size());
    if (!value.starts_with(prefix))
      return std::nullopt;
    return ValidateTelegramInitData(value.substr(prefix.size()), bot_token, std::chrono::system_clock::now(),
                                    config.auth_max_age);
  }

  http::response<http::string_body> Route(const http::request<http::string_body>& request) {
    const auto relative =
        RelativeTarget(std::string_view(request.target().data(), request.target().size()), config.public_path);
    if (!relative) {
      return ErrorResponse(http::status::not_found, "not_found", "route not found", request.version());
    }
    if (relative->empty()) {
      auto response = MakeResponse(http::status::permanent_redirect, {}, "text/plain", request.version());
      response.set(http::field::location, config.public_path);
      return response;
    }

    if (request.method() == http::verb::get) {
      if (*relative == "/health/live") {
        return JsonResponse(http::status::ok, boost::json::object{{"alive", true}}, request.version());
      }
      if (*relative == "/health/ready") {
        const bool assets = std::ifstream(std::filesystem::path(config.static_directory) / "index.html").good() &&
                            std::ifstream(std::filesystem::path(config.static_directory) / "app.js").good() &&
                            std::ifstream(std::filesystem::path(config.static_directory) / "style.css").good();
        const bool ready = assets && service.CheckReady();
        return JsonResponse(ready ? http::status::ok : http::status::service_unavailable,
                            boost::json::object{{"ready", ready}}, request.version());
      }
      std::string asset;
      if (*relative == "/" || *relative == "/index.html") {
        asset = "index.html";
      } else if (*relative == "/app.js") {
        asset = "app.js";
      } else if (*relative == "/style.css") {
        asset = "style.css";
      } else {
        return ErrorResponse(http::status::not_found, "not_found", "asset not found", request.version());
      }
      try {
        return MakeResponse(http::status::ok, ReadTextFile(std::filesystem::path(config.static_directory) / asset),
                            ContentType(asset), request.version());
      } catch (const std::exception&) {
        return ErrorResponse(http::status::not_found, "asset_missing", "static asset is not available",
                             request.version());
      }
    }

    if (request.method() != http::verb::post || !relative->starts_with("/api/v1/")) {
      return ErrorResponse(http::status::method_not_allowed, "method_not_allowed", "POST required", request.version());
    }

    const auto auth = Authenticate(request);
    if (!auth) {
      return ErrorResponse(http::status::unauthorized, "unauthorized", "invalid or expired Telegram initData",
                           request.version());
    }

    try {
      boost::json::object result;
      if (*relative == "/api/v1/bootstrap") {
        result = service.Bootstrap(auth->user, auth->start_param.value_or(""), auth->auth_date);
      } else if (*relative == "/api/v1/sessions") {
        result = service.StartSession(auth->user.id, ParseObjectBody(request));
      } else if (*relative == "/api/v1/session") {
        result = service.CurrentSession(auth->user.id);
      } else if (*relative == "/api/v1/session/resume") {
        result = service.ResumeSession(auth->user.id, ParseObjectBody(request));
      } else if (*relative == "/api/v1/session/answer") {
        result = service.SubmitAnswer(auth->user.id, ParseObjectBody(request));
      } else if (*relative == "/api/v1/session/share") {
        result = service.ShareChallenge(auth->user.id, ParseObjectBody(request));
      } else if (*relative == "/api/v1/session/match") {
        result = service.SelectMatching(auth->user.id, ParseObjectBody(request));
      } else if (*relative == "/api/v1/session/theory") {
        result = service.OpenTheory(auth->user.id, ParseObjectBody(request));
      } else if (*relative == "/api/v1/session/reference") {
        result = service.OpenReference(auth->user.id, ParseObjectBody(request));
      } else if (*relative == "/api/v1/session/next") {
        result = service.NextQuestion(auth->user.id, ParseObjectBody(request));
      } else if (*relative == "/api/v1/session/result") {
        result = service.SessionResult(auth->user.id, ParseObjectBody(request));
      } else if (*relative == "/api/v1/session/stop") {
        result = service.StopSession(auth->user.id, ParseObjectBody(request));
      } else if (*relative == "/api/v1/rewards/profile-frame") {
        result = service.SetProfileFrame(auth->user.id, ParseObjectBody(request));
      } else if (*relative == "/api/v1/training-presets") {
        result = service.SaveTrainingPreset(auth->user.id, ParseObjectBody(request));
      } else if (*relative == "/api/v1/card-feedback") {
        result = service.SubmitCardFeedback(auth->user.id, ParseObjectBody(request));
      } else if (*relative == "/api/v1/admin/card-feedback") {
        result = service.ListCardFeedback(auth->user.id, ParseObjectBody(request));
      } else if (*relative == "/api/v1/admin/card-feedback/review") {
        result = service.ReviewCardFeedback(auth->user.id, ParseObjectBody(request));
      } else if (*relative == "/api/v1/activity") {
        result = service.RecordActivity(auth->user.id, ParseObjectBody(request));
      } else if (*relative == "/api/v1/admin/statistics") {
        result = service.Statistics(auth->user.id, ParseObjectBody(request));
        result["technical"] = RuntimeMetrics::Instance().Snapshot();
        result["technical"].as_object()["pool_size"] = pool->Size();
        result["technical"].as_object()["pool_available"] = pool->Available();
        if (connections) {
          const auto executor = connections->Snapshot();
          result["technical"].as_object()["executor"] = boost::json::object{{"workers", executor.workers},
                                                                            {"active", executor.active},
                                                                            {"pending", executor.pending},
                                                                            {"pending_limit", executor.pending_limit},
                                                                            {"rejected", executor.rejected}};
        }
        result["maintenance"] = nullptr;
        if (const auto file = Environment("MINI_APP_MAINTENANCE_STATUS_FILE")) {
          std::ifstream input(*file);
          if (input) {
            boost::system::error_code error;
            const auto status = boost::json::parse(input, error);
            if (!error && status.is_object())
              result["maintenance"] = status;
          }
        }
      } else {
        return ErrorResponse(http::status::not_found, "not_found", "API route not found", request.version());
      }
      return JsonResponse(http::status::ok, std::move(result), request.version());
    } catch (const FeedbackAccessDenied& error) {
      return ErrorResponse(http::status::forbidden, "forbidden", error.what(), request.version());
    } catch (const FeedbackRateLimitError& error) {
      return ErrorResponse(http::status::too_many_requests, "rate_limited", error.what(), request.version());
    } catch (const std::invalid_argument& error) {
      return ErrorResponse(http::status::bad_request, "invalid_request", error.what(), request.version());
    } catch (const std::logic_error& error) {
      return ErrorResponse(http::status::conflict, "invalid_state", error.what(), request.version());
    }
  }

  http::response<http::string_body> InternalError(unsigned int version, std::string_view request_id,
                                                  std::string_view category) {
    spdlog::error("{}", boost::json::serialize(boost::json::object{{"event", "http_failure"},
                                                                   {"request_id", request_id},
                                                                   {"error_id", request_id},
                                                                   {"category", category},
                                                                   {"version", CPP_RECALL_BUILD_VERSION}}));
    return JsonResponse(http::status::internal_server_error,
                        boost::json::object{{"error", "internal_error"},
                                            {"message", "Не удалось обработать запрос. Попробуй ещё раз."},
                                            {"error_id", request_id}},
                        version);
  }

  void HandleConnection(tcp::socket socket, const std::stop_token& stop_token) {
    const auto started = std::chrono::steady_clock::now();
    const auto request_id = RuntimeMetrics::Instance().RequestId();
    socket.non_blocking(true);
    beast::flat_buffer buffer;
    http::request_parser<http::string_body> parser;
    parser.body_limit(kMaxRequestBody);
    boost::system::error_code error;
    SocketOperation(
        socket, POLLIN, stop_token, [&](auto& result) { http::read(socket, buffer, parser, result); }, error);
    if (error) {
      if (!stop_token.stop_requested()) {
        RuntimeMetrics::Instance().ReadFailure();
        spdlog::warn("event=http_read outcome=failed request_id={} category={} version={}", request_id,
                     error == asio::error::timed_out ? "deadline" : "invalid_or_closed", CPP_RECALL_BUILD_VERSION);
      }
      return;
    }
    auto response = [&] {
      try {
        return Route(parser.get());
      } catch (const std::bad_alloc&) {
        return InternalError(parser.get().version(), request_id, "resource_exhausted");
      } catch (const boost::system::system_error&) {
        return InternalError(parser.get().version(), request_id, "io_error");
      } catch (const std::exception&) {
        return InternalError(parser.get().version(), request_id, "operation_failed");
      } catch (...) {
        return InternalError(parser.get().version(), request_id, "unexpected_exception");
      }
    }();
    const auto relative = RelativeTarget(std::string_view(parser.get().target().data(), parser.get().target().size()),
                                         config.public_path);
    const auto route = MetricRoute(relative ? *relative : "other");
    const auto elapsed =
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started).count();
    RuntimeMetrics::Instance().Request(route, response.result_int(), elapsed);
    response.set("X-Request-ID", request_id);
    spdlog::info("{}", boost::json::serialize(boost::json::object{{"event", "http_request"},
                                                                  {"request_id", request_id},
                                                                  {"route", route},
                                                                  {"status", response.result_int()},
                                                                  {"duration_ms", elapsed},
                                                                  {"version", CPP_RECALL_BUILD_VERSION}}));
    http::response_serializer<http::string_body> serializer(response);
    SocketOperation(socket, POLLOUT, stop_token, [&](auto& result) { http::write(socket, serializer, result); }, error);
    if (error && !stop_token.stop_requested()) {
      RuntimeMetrics::Instance().WriteFailure();
      spdlog::warn("event=http_write outcome=failed request_id={} category=closed_or_deadline version={}", request_id,
                   CPP_RECALL_BUILD_VERSION);
    }
    socket.shutdown(tcp::socket::shutdown_send, error);
  }
};

MiniAppHttpServer::MiniAppHttpServer(MiniAppServerConfig config, std::string bot_token, const QuestionBank& questions,
                                     std::shared_ptr<DatabaseConnectionPool> connection_pool)
    : impl_(std::make_unique<Impl>(std::move(config), std::move(bot_token), questions, std::move(connection_pool))) {}

MiniAppHttpServer::~MiniAppHttpServer() = default;

const MiniAppServerConfig& MiniAppHttpServer::Config() const noexcept {
  return impl_->config;
}

void MiniAppHttpServer::Run(const std::stop_token& stop_token) {
  asio::io_context context;
  const auto address = asio::ip::make_address(impl_->config.listen_address);
  tcp::acceptor acceptor(context, tcp::endpoint(address, impl_->config.port));
  acceptor.non_blocking(true);
  spdlog::info("Mini App server listening on {}:{}, public_url={}", impl_->config.listen_address, impl_->config.port,
               impl_->config.public_url);

  constexpr std::size_t kPendingConnectionsPerWorker = 8;
  ConnectionWorkers connections{
      .executor = std::make_shared<BoundedExecutor>(impl_->config.worker_count,
                                                    impl_->config.worker_count * kPendingConnectionsPerWorker),
      .shutdown = {}};
  impl_->connections = connections.executor;
  std::stop_callback stop_connections(stop_token, [&] { connections.shutdown.request_stop(); });

  std::mutex wait_mutex;
  std::condition_variable_any wait_cv;
  while (!stop_token.stop_requested()) {
    tcp::socket socket(context);
    boost::system::error_code error;
    acceptor.accept(socket, error);
    if (!error) {
      const bool accepted =
          impl_->connections->TrySubmit([impl = impl_.get(), socket = std::move(socket),
                                         connection_stop = connections.shutdown.get_token()]() mutable {
            impl->HandleConnection(std::move(socket), connection_stop);
          });
      if (!accepted) {
        RuntimeMetrics::Instance().RejectConnection();
        // The rejected task owns the moved socket and closes it on destruction.
      }
      continue;
    }
    if (error != asio::error::would_block && error != asio::error::try_again) {
      throw boost::system::system_error(error);
    }
    std::unique_lock lock(wait_mutex);
    wait_cv.wait_for(lock, stop_token, std::chrono::milliseconds(25), [] { return false; });
  }
}
