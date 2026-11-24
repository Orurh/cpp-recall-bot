#include "reference_catalog.h"

#include "learning/skill_catalog.h"
#include "quiz/question.h"
#include "quiz/question_identity.h"

#include <boost/json.hpp>

#include <algorithm>
#include <stdexcept>
#include <unordered_set>
#include <utility>

namespace {

#include "learning/reference_catalog_data.inc"

}  // namespace

std::span<const ReferenceEntry> ReferenceCatalog() {
  return kEntries;
}

std::span<const ReferenceBinding> ReferenceBindingCatalog() {
  return kBindings;
}

std::string ReferenceContentSnapshot(const ReferenceEntry& entry) {
  boost::json::array sources;
  for (const auto source : entry.sources) {
    sources.emplace_back(source);
  }
  boost::json::object content{{"id", entry.id},
                              {"title", entry.title},
                              {"principle", entry.principle},
                              {"mechanism", entry.mechanism},
                              {"example", entry.example},
                              {"common_mistake", entry.common_mistake},
                              {"sources", std::move(sources)}};
  // Preserve existing article revisions and saved help provenance.
  if (entry.source_kind == LearningSourceKind::kDesignPrinciple)
    content["source_kind"] = "design_principle";
  return CanonicalQuestionJson(content);
}

std::string ReferenceContentRevision(const ReferenceEntry& entry) {
  return QuestionContentRevision(ReferenceContentSnapshot(entry));
}

void ValidateReferenceCatalog(std::span<const Question> questions) {
  for (const auto& [skill_id, topic] : kMaterialOwners) {
    const auto* skill = FindSkill(skill_id);
    if (skill == nullptr || skill->topic != topic)
      throw std::invalid_argument("unknown or misplaced material owner: " + std::string(skill_id));
  }
  std::unordered_set<std::string_view> reference_ids;
  for (const auto& entry : kEntries) {
    if (entry.id.empty() || !reference_ids.insert(entry.id).second || entry.title.empty() || entry.principle.empty() ||
        entry.mechanism.empty() || entry.example.empty() || entry.common_mistake.empty() || entry.sources.empty()) {
      throw std::invalid_argument("incomplete or duplicate reference: " + std::string(entry.id));
    }
    for (const auto source : entry.sources) {
      if (!IsLearningSource(source, entry.source_kind)) {
        throw std::invalid_argument("reference must identify a primary source for its subject: " +
                                    std::string(entry.id));
      }
    }
  }
  std::unordered_set<std::string_view> question_ids;
  for (const auto& binding : kBindings) {
    const auto question = std::ranges::find(questions, binding.question_id, &Question::id);
    if (!question_ids.insert(binding.question_id).second || question == questions.end() ||
        binding.content_revision != question->content_revision || binding.content_revision.empty() ||
        binding.reference_ids.empty()) {
      throw std::invalid_argument("missing, stale or duplicate reference binding: " + std::string(binding.question_id));
    }
    std::unordered_set<std::string_view> seen;
    for (const auto id : binding.reference_ids) {
      if (!reference_ids.contains(id) || !seen.insert(id).second) {
        throw std::invalid_argument("unknown or duplicate reference in binding: " + std::string(binding.question_id));
      }
    }
  }
}

std::vector<const ReferenceEntry*> ReferencesForQuestion(const Question& question) {
  std::vector<const ReferenceEntry*> result;
  const auto binding = std::ranges::find(kBindings, question.id, &ReferenceBinding::question_id);
  // Existing/synthetic sessions must fail closed if a card changed after review.
  if (binding == kBindings.end() || binding->content_revision != question.content_revision) {
    return result;
  }
  for (const auto id : binding->reference_ids) {
    const auto entry = std::ranges::find(kEntries, id, &ReferenceEntry::id);
    if (entry == kEntries.end()) {
      return {};
    }
    result.push_back(&*entry);
  }
  return result;
}
