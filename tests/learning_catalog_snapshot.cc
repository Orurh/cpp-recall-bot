// Include the real catalog translation units so the migration test can also pin
// private family bindings without adding a test-only production API.
#include "../src/learning/reference_catalog.cc"
#include "../src/learning/theory_catalog.cc"

#include "storage/question_loader.h"

#include <algorithm>
#include <fstream>
#include <iostream>
#include <iterator>

namespace {

boost::json::array Strings(const auto& values) {
  boost::json::array result;
  for (const auto value : values)
    result.emplace_back(value);
  return result;
}

boost::json::object CatalogSnapshot() {
  boost::json::array theory, references, theory_bindings, reference_bindings, questions, selections;
  for (const auto& block : TheoryCatalog()) {
    theory.emplace_back(boost::json::object{
        {"id", block.id},
        {"skill_id", block.skill_id},
        {"title", block.title},
        {"principle", block.principle},
        {"mechanism", block.mechanism},
        {"example", block.example},
        {"common_mistake", block.common_mistake},
        {"sources", Strings(block.sources)},
        {"source_kind", block.source_kind == LearningSourceKind::kCppStandard ? "cpp_standard" : "design_principle"},
        {"primary", block.primary}});
  }
  for (const auto& entry : ReferenceCatalog()) {
    references.emplace_back(boost::json::object{
        {"id", entry.id},
        {"title", entry.title},
        {"principle", entry.principle},
        {"mechanism", entry.mechanism},
        {"example", entry.example},
        {"common_mistake", entry.common_mistake},
        {"sources", Strings(entry.sources)},
        {"source_kind", entry.source_kind == LearningSourceKind::kCppStandard ? "cpp_standard" : "design_principle"},
        {"snapshot", ReferenceContentSnapshot(entry)},
        {"revision", ReferenceContentRevision(entry)}});
  }
  for (const auto& binding : kTheoryBindings)
    theory_bindings.emplace_back(boost::json::object{{"skill_id", binding.skill_id},
                                                     {"question_family_id", binding.question_family_id},
                                                     {"theory_id", binding.theory_id}});
  for (const auto& binding : ReferenceBindingCatalog())
    reference_bindings.emplace_back(boost::json::object{{"question_id", binding.question_id},
                                                        {"content_revision", binding.content_revision},
                                                        {"reference_ids", Strings(binding.reference_ids)}});
  const auto cards = LoadQuestions("content/questions");
  ValidateTheoryCatalog();
  ValidateReferenceCatalog(cards);
  for (const auto& card : cards) {
    questions.emplace_back(boost::json::object{
        {"id", card.id}, {"revision", card.content_revision}, {"snapshot", card.content_snapshot_json}});
    boost::json::array ids;
    for (const auto* entry : ReferencesForQuestion(card))
      ids.emplace_back(entry->id);
    const auto* block = FindTheoryForQuestion(card.skill_id, card.question_family_id);
    selections.emplace_back(
        boost::json::object{{"id", card.id},
                            {"references", std::move(ids)},
                            {"theory", block ? boost::json::value(block->id) : boost::json::value(nullptr)}});
  }
  return {{"theory", std::move(theory)},
          {"references", std::move(references)},
          {"theory_bindings", std::move(theory_bindings)},
          {"reference_bindings", std::move(reference_bindings)},
          {"questions", std::move(questions)},
          {"selections", std::move(selections)}};
}

}  // namespace

int main(int argc, char* argv[]) {
  try {
    const auto snapshot = CatalogSnapshot();
    const auto canonical = CanonicalQuestionJson(snapshot);
    if (argc == 2 && std::string_view(argv[1]) == "--dump") {
      std::cout << canonical << '\n';
      return 0;
    }
    if (argc != 2)
      throw std::invalid_argument("usage: learning_catalog_snapshot --dump|legacy_fixture.json");
    std::ifstream input(argv[1]);
    if (!input)
      throw std::runtime_error("cannot open legacy catalog fixture");
    const auto fixture = boost::json::parse(input).as_object();
    if (const auto* groups = fixture.if_contains("material_groups")) {
      for (const auto& item : groups->as_object()) {
        const auto& expected = item.value().as_object();
        const auto count = expected.at("count").to_number<std::size_t>();
        const auto& actual = snapshot.at(item.key()).as_array();
        if (actual.size() < count)
          throw std::runtime_error("missing legacy material: " + std::string(item.key()));
        boost::json::array prefix;
        for (std::size_t index = 0; index < count; ++index)
          prefix.push_back(actual[index]);
        // Only exact successor-reviewed legacy bindings may be normalized for
        // the frozen baseline. All other historical bytes are still compared.
        if (item.key() == "reference_bindings") {
          std::ifstream quality_input("docs/reviews/references/2026-11-24-quality-fixes.json");
          if (!quality_input)
            throw std::runtime_error("missing reviewed quality successor file");
          const std::string quality_text{std::istreambuf_iterator<char>(quality_input),
                                         std::istreambuf_iterator<char>()};
          const auto quality = boost::json::parse(quality_text).as_object();
          auto replacements = quality.at("cards").as_array();
          std::ifstream map_input("docs/reviews/references/2026-11-24-map-qa-batch02.json");
          if (!map_input)
            throw std::runtime_error("missing map QA reference successor file");
          const std::string map_text{std::istreambuf_iterator<char>(map_input), std::istreambuf_iterator<char>()};
          const auto map_qa = boost::json::parse(map_text).as_object();
          for (const auto& entry : map_qa.at("cards").as_array())
            replacements.push_back(entry);
          std::size_t normalized = 0;
          for (auto& entry : prefix) {
            auto& current = entry.as_object();
            const std::string id(current.at("question_id").as_string());
            if (id != "ranges_sort_list_constraints" && id != "vector_raii_early_return" &&
                id != "map_find_iterator_completion" &&
                id != "map_contains_completion" &&
                id != "map_try_emplace_completion" &&
                id != "map_insert_or_assign_completion" &&
                id != "review_cache_read_contract" &&
                id != "review_mark_existing_contract" &&
                id != "map_lookup_methods_matching" &&
                id != "map_subscript_inserts_default_value" &&
                id != "map_find_without_insertion" &&
                id != "map_insert_or_assign_vs_try_emplace" &&
                id != "map_update_existing_stock")
              continue;
            const auto found = std::ranges::find_if(replacements, [&](const auto& item) {
              return item.as_object().at("question_id") == current.at("question_id");
            });
            if (found == replacements.end())
              throw std::runtime_error("missing exact reviewed legacy binding replacement");
            const auto& reviewed = found->as_object();
            if (current.at("content_revision") != reviewed.at("content_revision") ||
                current.at("reference_ids") != reviewed.at("reference_ids"))
              throw std::runtime_error("unreviewed legacy binding change: " + id);
            current["content_revision"] = reviewed.at("previous_content_revision");
            current["reference_ids"] = reviewed.at("previous_reference_ids");
            ++normalized;
          }
          if (normalized != 13)
            throw std::runtime_error("unexpected legacy reference-binding override scope");
        }
        if (item.key() == "theory_bindings") {
          std::ifstream map_input("docs/reviews/theory/2026-11-24-map-qa-batch02.json");
          if (!map_input)
            throw std::runtime_error("missing map QA theory successor file");
          const std::string map_text{std::istreambuf_iterator<char>(map_input), std::istreambuf_iterator<char>()};
          const auto map_qa = boost::json::parse(map_text).as_object();
          const auto& bindings = map_qa.at("bindings").as_array();
          std::size_t normalized = 0;
          for (auto& entry : prefix) {
            auto& current = entry.as_object();
            const auto found = std::ranges::find_if(bindings, [&](const auto& value) {
              const auto& reviewed = value.as_object();
              return current.at("skill_id") == reviewed.at("skill_id") &&
                     current.at("question_family_id") == reviewed.at("question_family_id");
            });
            if (found == bindings.end())
              continue;
            const auto& reviewed = found->as_object();
            if (current.at("theory_id") != reviewed.at("theory_id"))
              throw std::runtime_error("unreviewed map theory binding");
            current["theory_id"] = reviewed.at("previous_theory_id");
            ++normalized;
          }
          if (normalized != 5)
            throw std::runtime_error("unexpected legacy map theory override scope");
        }
        if (expected.at("sha256").as_string() != QuestionContentRevision(CanonicalQuestionJson(prefix)))
          throw std::runtime_error("legacy material bytes or order changed: " + std::string(item.key()));
      }
    } else {
      if (fixture.at("catalog_sha256").as_string() != QuestionContentRevision(canonical))
        throw std::runtime_error("compiled learning catalog or canonical question snapshots differ from legacy");
      for (const auto& count : fixture.at("counts").as_object())
        if (snapshot.at(count.key()).as_array().size() != count.value().to_number<std::size_t>())
          throw std::runtime_error("legacy catalog count changed");
    }
    std::cout << "compiled learning catalog matches legacy snapshots and SHA-256\n";
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
