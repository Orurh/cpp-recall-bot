#include "quiz/question_identity.h"

#include <openssl/evp.h>
#include <boost/json.hpp>

#include <algorithm>
#include <array>
#include <stdexcept>
#include <vector>

namespace {

boost::json::value Canonicalize(const boost::json::value& value) {
  if (value.is_object()) {
    std::vector<std::string_view> keys;
    for (const auto& entry : value.as_object()) {
      keys.emplace_back(entry.key().data(), entry.key().size());
    }
    std::ranges::sort(keys);
    boost::json::object result;
    for (const auto key : keys) {
      result[key] = Canonicalize(value.as_object().at(key));
    }
    return result;
  }
  if (value.is_array()) {
    boost::json::array result;
    for (const auto& item : value.as_array()) {
      result.push_back(Canonicalize(item));
    }
    return result;
  }
  return value;
}

}  // namespace

std::string CanonicalQuestionJson(const boost::json::value& content) {
  return boost::json::serialize(Canonicalize(content));
}

std::string QuestionContentRevision(std::string_view canonical_json) {
  std::array<unsigned char, EVP_MAX_MD_SIZE> digest{};
  unsigned int size = 0;
  if (EVP_Digest(canonical_json.data(), canonical_json.size(), digest.data(), &size, EVP_sha256(), nullptr) != 1) {
    throw std::runtime_error("failed to hash question content");
  }
  constexpr std::string_view hex = "0123456789abcdef";
  std::string result;
  result.reserve(size * 2);
  for (unsigned int i = 0; i < size; ++i) {
    result.push_back(hex[digest[i] >> 4]);
    result.push_back(hex[digest[i] & 15]);
  }
  return result;
}

bool IsPublicQuestionId(std::string_view id) {
  return id.size() == 9 && id.starts_with("CR-") && id != "CR-000000" &&
         std::ranges::all_of(id.substr(3), [](char ch) { return ch >= '0' && ch <= '9'; });
}
