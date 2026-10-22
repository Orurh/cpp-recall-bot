#include "miniapp/telegram_init_data.h"

#include <boost/json.hpp>

#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <openssl/hmac.h>

#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <climits>
#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace {

constexpr std::size_t kSha256Size = 32;
constexpr std::chrono::seconds kAllowedClockSkew{60};

int HexDigit(char value) {
  if (value >= '0' && value <= '9')
    return value - '0';
  if (value >= 'a' && value <= 'f')
    return value - 'a' + 10;
  if (value >= 'A' && value <= 'F')
    return value - 'A' + 10;
  return -1;
}

std::optional<std::string> PercentDecode(std::string_view input) {
  std::string output;
  output.reserve(input.size());

  for (std::size_t index = 0; index < input.size(); ++index) {
    const char current = input[index];
    if (current == '+') {
      output.push_back(' ');
      continue;
    }
    if (current != '%') {
      output.push_back(current);
      continue;
    }
    if (index + 2 >= input.size())
      return std::nullopt;
    const int high = HexDigit(input[index + 1]);
    const int low = HexDigit(input[index + 2]);
    if (high < 0 || low < 0)
      return std::nullopt;
    output.push_back(static_cast<char>((high << 4) | low));
    index += 2;
  }

  return output;
}

std::optional<std::map<std::string, std::string>> ParseQuery(std::string_view query) {
  std::map<std::string, std::string> fields;
  std::size_t offset = 0;

  while (offset <= query.size()) {
    const auto ampersand = query.find('&', offset);
    const auto end = ampersand == std::string_view::npos ? query.size() : ampersand;
    const auto part = query.substr(offset, end - offset);
    const auto equals = part.find('=');
    if (part.empty() || equals == std::string_view::npos) {
      return std::nullopt;
    }

    auto key = PercentDecode(part.substr(0, equals));
    auto value = PercentDecode(part.substr(equals + 1));
    if (!key || !value || key->empty() || fields.contains(*key)) {
      return std::nullopt;
    }
    fields.emplace(std::move(*key), std::move(*value));

    if (ampersand == std::string_view::npos)
      break;
    offset = ampersand + 1;
  }

  return fields;
}

std::optional<std::array<unsigned char, kSha256Size>> DecodeHash(std::string_view hex) {
  if (hex.size() != kSha256Size * 2)
    return std::nullopt;
  std::array<unsigned char, kSha256Size> bytes{};
  for (std::size_t index = 0; index < bytes.size(); ++index) {
    const int high = HexDigit(hex[index * 2]);
    const int low = HexDigit(hex[index * 2 + 1]);
    if (high < 0 || low < 0)
      return std::nullopt;
    bytes[index] = static_cast<unsigned char>((high << 4) | low);
  }
  return bytes;
}

std::optional<std::array<unsigned char, kSha256Size>> HmacSha256(std::string_view key, std::string_view value) {
  if (key.size() > static_cast<std::size_t>(INT_MAX))
    return std::nullopt;
  std::array<unsigned char, EVP_MAX_MD_SIZE> digest{};
  unsigned int size = 0;
  if (HMAC(EVP_sha256(), key.data(), static_cast<int>(key.size()), reinterpret_cast<const unsigned char*>(value.data()),
           value.size(), digest.data(), &size) == nullptr ||
      size != kSha256Size) {
    return std::nullopt;
  }
  std::array<unsigned char, kSha256Size> result{};
  std::copy_n(digest.begin(), result.size(), result.begin());
  return result;
}

std::optional<std::int64_t> ParseInt64(std::string_view value) {
  std::int64_t result = 0;
  const auto [end, error] = std::from_chars(value.data(), value.data() + value.size(), result);
  if (error != std::errc{} || end != value.data() + value.size()) {
    return std::nullopt;
  }
  return result;
}

std::string JsonString(const boost::json::object& object, std::string_view key) {
  const auto* value = object.if_contains(key);
  if (value == nullptr || !value->is_string())
    return {};
  const auto& text = value->as_string();
  return {text.data(), text.size()};
}

}  // namespace

std::optional<TelegramInitData> ValidateTelegramInitData(std::string_view raw_init_data, std::string_view bot_token,
                                                         std::chrono::system_clock::time_point now,
                                                         std::chrono::seconds max_age) {
  if (raw_init_data.empty() || raw_init_data.size() > 16 * 1024 || bot_token.empty() ||
      max_age <= std::chrono::seconds::zero()) {
    return std::nullopt;
  }

  auto fields = ParseQuery(raw_init_data);
  if (!fields)
    return std::nullopt;

  const auto hash_it = fields->find("hash");
  if (hash_it == fields->end())
    return std::nullopt;
  const auto received_hash = DecodeHash(hash_it->second);
  if (!received_hash)
    return std::nullopt;
  fields->erase(hash_it);

  std::string check_string;
  for (const auto& [key, value] : *fields) {
    if (!check_string.empty())
      check_string.push_back('\n');
    check_string += key;
    check_string.push_back('=');
    check_string += value;
  }

  const auto secret = HmacSha256("WebAppData", bot_token);
  if (!secret)
    return std::nullopt;
  const std::string_view secret_view(reinterpret_cast<const char*>(secret->data()), secret->size());
  const auto calculated_hash = HmacSha256(secret_view, check_string);
  if (!calculated_hash || CRYPTO_memcmp(received_hash->data(), calculated_hash->data(), received_hash->size()) != 0) {
    return std::nullopt;
  }

  const auto auth_it = fields->find("auth_date");
  const auto user_it = fields->find("user");
  if (auth_it == fields->end() || user_it == fields->end()) {
    return std::nullopt;
  }
  const auto auth_date = ParseInt64(auth_it->second);
  if (!auth_date || *auth_date <= 0)
    return std::nullopt;
  const auto now_seconds = std::chrono::duration_cast<std::chrono::seconds>(now.time_since_epoch()).count();
  if (now_seconds < 0) {
    return std::nullopt;
  }
  if (*auth_date > now_seconds) {
    if (*auth_date - now_seconds > kAllowedClockSkew.count()) {
      return std::nullopt;
    }
  } else if (now_seconds - *auth_date > max_age.count()) {
    return std::nullopt;
  }

  boost::system::error_code error;
  const auto user_json = boost::json::parse(user_it->second, error);
  if (error || !user_json.is_object())
    return std::nullopt;
  const auto& user_object = user_json.as_object();
  const auto* id = user_object.if_contains("id");
  if (id == nullptr || !id->is_int64() || id->as_int64() <= 0) {
    return std::nullopt;
  }

  TelegramInitData result{
      .user =
          TelegramMiniAppUser{
              .id = id->as_int64(),
              .first_name = JsonString(user_object, "first_name"),
              .last_name = JsonString(user_object, "last_name"),
              .username = JsonString(user_object, "username"),
              .language_code = JsonString(user_object, "language_code"),
          },
      .auth_date = *auth_date,
      .query_id = std::nullopt,
  };
  if (const auto query_it = fields->find("query_id"); query_it != fields->end() && !query_it->second.empty()) {
    result.query_id = query_it->second;
  }
  return result;
}
