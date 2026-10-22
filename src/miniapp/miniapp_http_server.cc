#include "miniapp/miniapp_http_server.h"

#include "miniapp/miniapp_service.h"
#include "miniapp/telegram_init_data.h"

#include <boost/asio.hpp>
#include <boost/beast.hpp>
#include <boost/json.hpp>

#include <spdlog/spdlog.h>

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
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

namespace {

namespace asio = boost::asio;
namespace beast = boost::beast;
namespace http = beast::http;
using tcp = asio::ip::tcp;

constexpr std::size_t kMaxRequestBody = 64 * 1024;

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
  if (const auto value = Environment("MINI_APP_AUTH_MAX_AGE_SECONDS")) {
    config.auth_max_age = ParseMaxAge(*value);
  }
  return config;
}

struct MiniAppHttpServer::Impl {
  Impl(MiniAppServerConfig server_config, std::string token, const QuestionBank& questions, std::string database_url)
      : config(std::move(server_config)), bot_token(std::move(token)), service(questions, std::move(database_url)) {}

  MiniAppServerConfig config;
  std::string bot_token;
  MiniAppService service;

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
        result = service.Bootstrap(auth->user);
      } else if (*relative == "/api/v1/sessions") {
        result = service.StartSession(auth->user.id, ParseObjectBody(request));
      } else if (*relative == "/api/v1/session") {
        result = service.CurrentSession(auth->user.id);
      } else if (*relative == "/api/v1/session/answer") {
        result = service.SubmitAnswer(auth->user.id, ParseObjectBody(request));
      } else if (*relative == "/api/v1/session/match") {
        result = service.SelectMatching(auth->user.id, ParseObjectBody(request));
      } else if (*relative == "/api/v1/session/next") {
        result = service.NextQuestion(auth->user.id);
      } else if (*relative == "/api/v1/session/stop") {
        result = service.StopSession(auth->user.id);
      } else {
        return ErrorResponse(http::status::not_found, "not_found", "API route not found", request.version());
      }
      return JsonResponse(http::status::ok, std::move(result), request.version());
    } catch (const std::invalid_argument& error) {
      return ErrorResponse(http::status::bad_request, "invalid_request", error.what(), request.version());
    } catch (const std::logic_error& error) {
      return ErrorResponse(http::status::conflict, "invalid_state", error.what(), request.version());
    } catch (const std::exception& error) {
      spdlog::error("Mini App request failed: {}", error.what());
      return ErrorResponse(http::status::internal_server_error, "internal_error", "request processing failed",
                           request.version());
    }
  }

  void HandleConnection(tcp::socket socket) {
    beast::tcp_stream stream(std::move(socket));
    stream.expires_after(std::chrono::seconds(10));
    beast::flat_buffer buffer;
    http::request_parser<http::string_body> parser;
    parser.body_limit(kMaxRequestBody);
    boost::system::error_code error;
    http::read(stream, buffer, parser, error);
    if (error)
      return;
    auto response = Route(parser.get());
    http::write(stream, response, error);
    stream.socket().shutdown(tcp::socket::shutdown_send, error);
  }
};

MiniAppHttpServer::MiniAppHttpServer(MiniAppServerConfig config, std::string bot_token, const QuestionBank& questions,
                                     std::string database_url)
    : impl_(std::make_unique<Impl>(std::move(config), std::move(bot_token), questions, std::move(database_url))) {}

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

  std::mutex wait_mutex;
  std::condition_variable_any wait_cv;
  while (!stop_token.stop_requested()) {
    tcp::socket socket(context);
    boost::system::error_code error;
    acceptor.accept(socket, error);
    if (!error) {
      impl_->HandleConnection(std::move(socket));
      continue;
    }
    if (error != asio::error::would_block && error != asio::error::try_again) {
      throw boost::system::system_error(error);
    }
    std::unique_lock lock(wait_mutex);
    wait_cv.wait_for(lock, stop_token, std::chrono::milliseconds(25), [] { return false; });
  }
}
