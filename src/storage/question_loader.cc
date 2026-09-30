#include "question_loader.h"

#include <boost/json.hpp>

#include <fstream>
#include <limits>
#include <stdexcept>
#include <utility>

namespace {

std::string ReadRequiredString(const boost::json::object& object, const char* key) {
  const auto& value = object.at(key).as_string();
  return {value.data(), value.size()};
}

std::optional<std::string> ReadOptionalString(const boost::json::object& object, const char* key) {
  const auto& value = object.at(key);
  if (value.is_null()) {
    return std::nullopt;
  }

  const auto& text = value.as_string();
  return std::string(text.data(), text.size());
}

int ReadLevel(const boost::json::object& object) {
  const auto kLevel = object.at("level").as_int64();
  if (kLevel < std::numeric_limits<int>::min() || kLevel > std::numeric_limits<int>::max()) {
    throw std::out_of_range("question level is outside int range");
  }

  return static_cast<int>(kLevel);
}

}  // namespace

Question LoadQuestion(const std::filesystem::path& path) {
  std::ifstream input(path);
  if (!input) {
    throw std::runtime_error("file does not exist" + path.string());
  }
  const auto kJson = boost::json::parse(input);  // преобразуем в boost::json
  const auto& object = kJson.as_object();        // это может быть как обычный объект, так и массив
  std::vector<AnswerOption> options;
  for (const auto& item : object.at("options").as_array()) {
    const auto& option = item.as_object();
    options.push_back({
        .id = ReadRequiredString(option, "id"),
        .text = ReadRequiredString(option, "text"),
    });
  }
  return {
      .id = ReadRequiredString(object, "id"),
      .topic = ReadRequiredString(object, "topic"),
      .level = ReadLevel(object),
      .standard = ReadRequiredString(object, "standard"),
      .text = ReadRequiredString(object, "text"),
      .code = ReadOptionalString(object, "code"),
      .options = std::move(options),
      .correct_option_id = ReadRequiredString(object, "correct_option_id"),
      .explanation = ReadRequiredString(object, "explanation"),
      .source = ReadOptionalString(object, "source"),
  };
}