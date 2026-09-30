#include "telegram_bot.h"

#include <boost/asio.hpp>
#include <boost/asio/ssl.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <boost/beast/ssl.hpp>
#include <boost/json.hpp>

#include <openssl/ssl.h>

#include <stdexcept>
#include <string>
#include <utility>

namespace net = boost::asio;
namespace ssl = net::ssl;
namespace beast = boost::beast;
namespace http = beast::http;
using Tcp = net::ip::tcp;

TelegramBot::TelegramBot(std::string token) : token_(std::move(token)) {
  if (token_.empty()) {
    throw std::invalid_argument("Telegram token is empty");
  }
}

boost::json::value TelegramBot::Call(std::string_view method, const boost::json::object& params) {
  if (method.empty() || method.find_first_not_of("abcdefghijklmnopqrstuvwxyz"
                                                 "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
                                                 "0123456789_") != std::string_view::npos) {
    throw std::invalid_argument("Invalid Telegram method name");
  }

  constexpr const char* kHost = "api.telegram.org";

  net::io_context io;
  ssl::context tls(ssl::context::tls_client);

  // Доверенные центры сертификации из настроек OpenSSL.
  tls.set_default_verify_paths();
  tls.set_verify_mode(ssl::verify_peer);

  Tcp::resolver resolver(io);
  beast::ssl_stream<beast::tcp_stream> stream(io, tls);

  // Проверяем, что сертификат выдан для api.telegram.org.
  stream.set_verify_callback(ssl::host_name_verification(kHost));

  // SNI: сообщаем серверу имя сайта при TLS handshake.
  if (SSL_set_tlsext_host_name(stream.native_handle(), kHost) != 1) {
    throw std::runtime_error("Failed to configure TLS SNI");
  }

  // DNS → TCP → TLS.
  const auto endpoints = resolver.resolve(kHost, "443");
  beast::get_lowest_layer(stream).connect(endpoints);
  stream.handshake(ssl::stream_base::client);

  const std::string target = "/bot" + token_ + "/" + std::string(method);

  http::request<http::string_body> request{http::verb::post, target, 11};

  request.set(http::field::host, kHost);
  request.set(http::field::user_agent, "cpp-recall-bot");
  request.set(http::field::content_type, "application/json");
  request.keep_alive(false);

  request.body() = boost::json::serialize(params);
  request.prepare_payload();

  http::write(stream, request);

  beast::flat_buffer buffer;
  http::response_parser<http::string_body> parser;

  // Ограничиваем размер ответа: 8 MiB.
  parser.body_limit(8ULL * 1024ULL * 1024ULL);
  http::read(stream, buffer, parser);

  auto response = parser.release();

  // Полный HTTP-ответ уже получен. Ошибка закрытия соединения
  // не должна превращать выполненный запрос в неудачный.
  boost::system::error_code shutdown_error;
  stream.shutdown(shutdown_error);

  boost::system::error_code parse_error;
  auto document = boost::json::parse(response.body(), parse_error);

  const auto status = response.result_int();

  if (parse_error || !document.is_object()) {
    throw std::runtime_error("Telegram returned invalid JSON; HTTP " + std::to_string(status));
  }

  const auto& object = document.as_object();
  const auto* ok = object.if_contains("ok");

  if (status < 200 || status >= 300 || ok == nullptr || !ok->is_bool() || !ok->as_bool()) {
    std::string message = "Telegram API error; HTTP " + std::to_string(status);

    const auto* description = object.if_contains("description");
    if (description != nullptr && description->is_string()) {
      const auto& text = description->as_string();
      message += ": ";
      message.append(text.data(), text.size());
    }

    throw std::runtime_error(message);
  }

  // Возвращаем самостоятельное значение, а не ссылку
  // на локальный document.
  return object.at("result");
}