#include "curl_http_client.h"

#include <curl/curl.h>

#include <array>
#include <chrono>
#include <cstddef>
#include <memory>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

namespace {

using namespace std::chrono_literals;

constexpr auto kConnectTimeout = 10s;
constexpr auto kRequestTimeout = 35s;

class CurlGlobal {
 public:
  CurlGlobal() {
    const auto result = curl_global_init(CURL_GLOBAL_DEFAULT);

    if (result != CURLE_OK) {
      throw std::runtime_error("curl_global_init failed");
    }
  }

  ~CurlGlobal() { curl_global_cleanup(); }

  CurlGlobal(const CurlGlobal&) = delete;
  CurlGlobal& operator=(const CurlGlobal&) = delete;
  CurlGlobal(CurlGlobal&&) = delete;
  CurlGlobal& operator=(CurlGlobal&&) = delete;
};

void EnsureCurlInitialized() {
  static const CurlGlobal global;
}

void CheckCurl(CURLcode result, const char* operation) {
  if (result == CURLE_OK) {
    return;
  }

  throw std::runtime_error(std::string(operation) + ": " + curl_easy_strerror(result));
}

std::size_t WriteCallback(char* data, std::size_t size, std::size_t count, void* user_data) noexcept {
  const auto bytes = size * count;

  try {
    auto& body = *static_cast<std::string*>(user_data);

    body.append(data, bytes);

    return bytes;
  } catch (...) {
    return 0;
  }
}

int TransferCallback(void* user_data, [[maybe_unused]] curl_off_t download_total,
                     [[maybe_unused]] curl_off_t download_now, [[maybe_unused]] curl_off_t upload_total,
                     [[maybe_unused]] curl_off_t upload_now) noexcept {
  const auto& stop_token = *static_cast<const std::stop_token*>(user_data);

  return stop_token.stop_requested() ? 1 : 0;
}

struct HeaderListDeleter {
  void operator()(curl_slist* headers) const noexcept { curl_slist_free_all(headers); }
};

}  // namespace

void CurlHttpClient::CurlDeleter::operator()(CURL* handle) const noexcept {
  if (handle != nullptr) {
    curl_easy_cleanup(handle);
  }
}

CurlHttpClient::CurlHttpClient() {
  EnsureCurlInitialized();

  handle_.reset(curl_easy_init());

  if (!handle_) {
    throw std::runtime_error("curl_easy_init failed");
  }
}

CurlHttpClient::Response CurlHttpClient::PostJson(std::string_view url, std::string_view body,
                                                  const std::stop_token& stop_token) {
  if (stop_token.stop_requested()) {
    throw std::system_error(std::make_error_code(std::errc::operation_canceled));
  }

  CURL* handle = handle_.get();

  curl_easy_reset(handle);

  std::string response_body;

  std::array<char, CURL_ERROR_SIZE> error_buffer{};

  curl_slist* raw_headers = nullptr;

  raw_headers = curl_slist_append(raw_headers, "Content-Type: application/json");

  if (raw_headers == nullptr) {
    throw std::runtime_error("curl_slist_append failed");
  }

  std::unique_ptr<curl_slist, HeaderListDeleter> headers(raw_headers);
  const std::string url_string(url);
  CheckCurl(curl_easy_setopt(handle, CURLOPT_URL, url_string.c_str()), "CURLOPT_URL");

  CheckCurl(curl_easy_setopt(handle, CURLOPT_POST, 1L), "CURLOPT_POST");

  const std::string request_body(body);

  CheckCurl(curl_easy_setopt(handle, CURLOPT_POSTFIELDS, request_body.c_str()), "CURLOPT_POSTFIELDS");

  CheckCurl(curl_easy_setopt(handle, CURLOPT_POSTFIELDSIZE_LARGE, static_cast<curl_off_t>(request_body.size())),
            "CURLOPT_POSTFIELDSIZE_LARGE");

  CheckCurl(curl_easy_setopt(handle, CURLOPT_HTTPHEADER, headers.get()), "CURLOPT_HTTPHEADER");

  CheckCurl(curl_easy_setopt(handle, CURLOPT_USERAGENT, "cpp-recall-bot"), "CURLOPT_USERAGENT");

  CheckCurl(curl_easy_setopt(handle, CURLOPT_CONNECTTIMEOUT_MS,
                             std::chrono::duration_cast<std::chrono::milliseconds>(kConnectTimeout).count()),
            "CURLOPT_CONNECTTIMEOUT_MS");

  CheckCurl(curl_easy_setopt(handle, CURLOPT_TIMEOUT_MS,
                             std::chrono::duration_cast<std::chrono::milliseconds>(kRequestTimeout).count()),
            "CURLOPT_TIMEOUT_MS");

  CheckCurl(curl_easy_setopt(handle, CURLOPT_NOSIGNAL, 1L), "CURLOPT_NOSIGNAL");

  CheckCurl(curl_easy_setopt(handle, CURLOPT_SSL_VERIFYPEER, 1L), "CURLOPT_SSL_VERIFYPEER");

  CheckCurl(curl_easy_setopt(handle, CURLOPT_SSL_VERIFYHOST, 2L), "CURLOPT_SSL_VERIFYHOST");

  CheckCurl(curl_easy_setopt(handle, CURLOPT_WRITEFUNCTION, WriteCallback), "CURLOPT_WRITEFUNCTION");

  CheckCurl(curl_easy_setopt(handle, CURLOPT_WRITEDATA, &response_body), "CURLOPT_WRITEDATA");

  CheckCurl(curl_easy_setopt(handle, CURLOPT_NOPROGRESS, 0L), "CURLOPT_NOPROGRESS");

  CheckCurl(curl_easy_setopt(handle, CURLOPT_XFERINFOFUNCTION, TransferCallback), "CURLOPT_XFERINFOFUNCTION");

  CheckCurl(curl_easy_setopt(handle, CURLOPT_XFERINFODATA, &stop_token), "CURLOPT_XFERINFODATA");

  CheckCurl(curl_easy_setopt(handle, CURLOPT_ERRORBUFFER, error_buffer.data()), "CURLOPT_ERRORBUFFER");

  const auto result = curl_easy_perform(handle);

  if (result == CURLE_ABORTED_BY_CALLBACK && stop_token.stop_requested()) {
    throw std::system_error(std::make_error_code(std::errc::operation_canceled));
  }

  if (result != CURLE_OK) {
    const std::string message =
        error_buffer.front() != '\0' ? std::string(error_buffer.data()) : std::string(curl_easy_strerror(result));

    throw std::runtime_error("HTTP request failed: " + message);
  }

  long status_code = 0;  // NOLINT(google-runtime-int)

  CheckCurl(curl_easy_getinfo(handle, CURLINFO_RESPONSE_CODE, &status_code), "CURLINFO_RESPONSE_CODE");

  return {
      .status_code = static_cast<std::int64_t>(status_code),
      .body = std::move(response_body),
  };
}