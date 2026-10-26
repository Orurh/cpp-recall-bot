#pragma once

#include <span>
#include <string>
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
};

struct ReferenceBinding {
  std::string_view question_id;
  std::string_view content_revision;
  std::vector<std::string_view> reference_ids;
};

[[nodiscard]] std::span<const ReferenceEntry> ReferenceCatalog();
[[nodiscard]] std::span<const ReferenceBinding> ReferenceBindingCatalog();
[[nodiscard]] std::string ReferenceContentSnapshot(const ReferenceEntry& entry);
[[nodiscard]] std::string ReferenceContentRevision(const ReferenceEntry& entry);
// Editorial consistency check for CI/content_check; not proof of technical truth.
void ValidateReferenceCatalog(std::span<const Question> questions);
[[nodiscard]] std::vector<const ReferenceEntry*> ReferencesForQuestion(const Question& question);
