#pragma once

#include <span>
#include <string_view>
#include <vector>

struct Question;

struct ReferenceEntry {
  std::string_view id;
  std::string_view title;
  std::string_view principle;
  std::string_view mechanism;
  std::string_view example;
  std::string_view common_mistake;
  std::vector<std::string_view> sources;
  std::vector<std::string_view> tags;
};

[[nodiscard]] std::span<const ReferenceEntry> ReferenceCatalog();
[[nodiscard]] std::vector<const ReferenceEntry*> ReferencesForQuestion(const Question& question);
