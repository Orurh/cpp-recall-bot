#include "learning/reference_catalog.h"
#include "quiz/question.h"
#include "storage/question_loader.h"

#include <boost/json.hpp>

#include <algorithm>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

namespace {

void Check(bool condition, std::string_view message) {
  if (!condition) {
    throw std::runtime_error(std::string(message));
  }
}

template <typename Operation>
void MustReject(Operation&& operation) {
  bool rejected = false;
  try {
    operation();
  } catch (const std::exception&) {
    rejected = true;
  }
  Check(rejected, "a stale, incomplete or incorrect reference review was accepted");
}

// Traceability and relevance decisions, not an automatic proof of C++ semantics.
void ValidateLedger(const boost::json::object& ledger, const std::vector<Question>& questions) {
  Check(ledger.at("schema_version").as_int64() == 1, "unsupported reference review schema");
  Check(ledger.at("standard_document").as_string() == "N4950", "missing reference standard edition");
  Check(!ledger.at("reviewed_at").as_string().empty(), "missing reference review date");
  Check(ledger.at("reviewer").as_object().at("mode").as_string() == "self_review" &&
            !ledger.at("reviewer").as_object().at("independent").as_bool(),
        "reference self-review misrepresented as independent audit");
  std::unordered_set<std::string> articles;
  for (const auto& value : ledger.at("references").as_array()) {
    const auto& record = value.as_object();
    const std::string id(record.at("id").as_string());
    Check(articles.insert(id).second, "duplicate reference review");
    const auto entry = std::ranges::find(ReferenceCatalog(), id, &ReferenceEntry::id);
    Check(entry != ReferenceCatalog().end(), "reviewed reference no longer exists");
    Check(record.at("content_revision").as_string() == ReferenceContentRevision(*entry),
          "reference text changed: recheck its affected bindings");
    Check(record.at("example").as_string() == entry->example, "reference example fixture diverged");
    const auto* kind = record.if_contains("source_kind");
    Check(kind == nullptr || kind->as_string() == "design_principle", "unknown article source kind");
    Check((kind != nullptr) == (entry->source_kind == LearningSourceKind::kDesignPrinciple),
          "design article was attributed to the language standard");
  }
  Check(articles.size() == ReferenceCatalog().size(), "unreviewed reference article");
  std::unordered_set<std::string> cards;
  for (const auto& value : ledger.at("cards").as_array()) {
    const auto& record = value.as_object();
    const std::string id(record.at("question_id").as_string());
    Check(cards.insert(id).second, "duplicate card in reference review");
    const auto question = std::ranges::find(questions, id, &Question::id);
    Check(question != questions.end(), "reviewed reference card no longer exists");
    Check(record.at("public_id").as_string() == question->public_id &&
              record.at("content_revision").as_string() == question->content_revision,
          "card changed: recheck its references");
    Check(!record.at("rationale").as_string().empty(), "missing relevance rationale");
    const auto actual = ReferencesForQuestion(*question);
    const auto& expected = record.at("reference_ids").as_array();
    Check(actual.size() == expected.size(), "unexpected or missing article attached to card");
    std::unordered_set<std::string> seen;
    for (std::size_t index = 0; index < actual.size(); ++index) {
      const std::string reference(expected[index].as_string());
      Check(seen.insert(reference).second && articles.contains(reference), "invalid reviewed reference id");
      Check(actual[index]->id == reference, "wrong article or order attached to card");
    }
  }
  Check(cards.size() == questions.size(), "reference matrix does not cover the current bank");
}

}  // namespace

void CheckReferenceBindings() {
  const auto questions = LoadQuestions("content/questions");
  ValidateReferenceCatalog(questions);
  for (const auto& question : questions)
    if (question.topic == "containers")
      Check(!ReferencesForQuestion(question).empty(), "containers card has no before-answer preparation");
  std::ifstream input("docs/reviews/references/2026-10-26-bindings.json");
  Check(input.is_open(), "cannot open reference review matrix");
  const std::string text{std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
  auto ledger = boost::json::parse(text).as_object();
  // Preserve the historical review. Later, independently dated supplements may
  // append articles/cards and replace ONLY explicitly pinned old revisions.
  std::ifstream supplement_input("docs/reviews/references/2026-11-23-content02-containers.json");
  Check(supplement_input.is_open(), "missing current reference review supplement");
  const std::string supplement_text{std::istreambuf_iterator<char>(supplement_input), std::istreambuf_iterator<char>()};
  const auto supplement = boost::json::parse(supplement_text).as_object();
  Check(supplement.at("schema_version") == 1 && supplement.at("standard_document") == "N4950" &&
            supplement.at("reviewed_at") == "2026-11-23" &&
            supplement.at("reviewer").as_object().at("mode") == "self_review" &&
            !supplement.at("reviewer").as_object().at("independent").as_bool(),
        "invalid current reference review provenance");
  for (const auto& entry : supplement.at("references").as_array()) {
    const auto id = entry.as_object().at("id");
    Check(std::ranges::none_of(ledger.at("references").as_array(),
                               [&](const auto& old) { return old.as_object().at("id") == id; }),
          "supplement rewrote an old reference review");
    ledger.at("references").as_array().push_back(entry);
  }
  const auto& replacements = supplement.at("replaces_card_revisions").as_object();
  Check(replacements.size() == 1 && replacements.contains("stable_sort_equivalent_order"),
        "unexpected historical reference replacement");
  for (const auto& entry : supplement.at("cards").as_array()) {
    const auto id = std::string(entry.as_object().at("question_id").as_string());
    auto& cards = ledger.at("cards").as_array();
    const auto old = std::ranges::find_if(
        cards, [&](const auto& record) { return record.as_object().at("question_id").as_string() == id; });
    if (old != cards.end()) {
      const auto* expected = replacements.if_contains(id);
      Check(expected && old->as_object().at("content_revision") == *expected,
            "supplement replaced an unpinned historical card");
      *old = entry;
    } else {
      Check(!replacements.contains(id), "supplement replacement has no original card");
      cards.push_back(entry);
    }
  }
  std::ifstream preparation_input("docs/reviews/references/2026-11-24-containers-preparation.json");
  Check(preparation_input.is_open(), "missing containers preparation review");
  const std::string preparation_text{std::istreambuf_iterator<char>(preparation_input),
                                     std::istreambuf_iterator<char>()};
  const auto preparation = boost::json::parse(preparation_text).as_object();
  Check(preparation.at("schema_version") == 1 && preparation.at("standard_document") == "N4950" &&
            preparation.at("reviewed_at") == "2026-11-24" &&
            preparation.at("reviewer").as_object().at("mode") == "self_review" &&
            !preparation.at("reviewer").as_object().at("independent").as_bool() &&
            preparation.at("references").as_array().size() == 8 && preparation.at("cards").as_array().size() == 8,
        "invalid preparation review scope or provenance");
  for (const auto& entry : preparation.at("references").as_array()) {
    Check(std::ranges::none_of(ledger.at("references").as_array(),
                               [&](const auto& old) { return old.as_object().at("id") == entry.as_object().at("id"); }),
          "preparation rewrote an old article");
    ledger.at("references").as_array().push_back(entry);
  }
  for (const auto& entry : preparation.at("cards").as_array()) {
    const auto& record = entry.as_object();
    auto& cards = ledger.at("cards").as_array();
    const auto old = std::ranges::find_if(
        cards, [&](const auto& item) { return item.as_object().at("question_id") == record.at("question_id"); });
    Check(old != cards.end() && old->as_object().at("content_revision") == record.at("content_revision") &&
              old->as_object().at("reference_ids") == record.at("previous_reference_ids") &&
              record.at("previous_reference_ids").as_array().empty(),
          "preparation changed a card revision or overwrote existing help");
    *old = entry;
  }
  // MOVE-01 reviews explicitly pin both articles and their exact question revisions.
  std::ifstream move_input("docs/reviews/references/2026-11-24-move01.json");
  Check(move_input.is_open(), "missing MOVE-01 before-answer review");
  const std::string move_text{std::istreambuf_iterator<char>(move_input), std::istreambuf_iterator<char>()};
  const auto move_review = boost::json::parse(move_text).as_object();
  const auto& move_references = move_review.at("references").as_array();
  const auto& move_cards = move_review.at("cards").as_array();
  const auto& reviewer = move_review.at("reviewer").as_object();
  const bool valid_provenance = move_review.at("schema_version") == 1 &&
                                move_review.at("standard_document") == "N4950" &&
                                reviewer.at("mode") == "self_review" && !reviewer.at("independent").as_bool();
  Check(valid_provenance && move_references.size() == 2 && move_cards.size() == 2,
        "unexpected MOVE-01 review provenance or scope");
  std::unordered_set<std::string> move_articles;
  for (const auto& value : move_references) {
    const auto& entry = value.as_object();
    const std::string id(entry.at("id").as_string());
    const bool known_article = id == "move01.named_reference_category" || id == "move01.noexcept_selection";
    Check(known_article && move_articles.insert(id).second, "unexpected or repeated MOVE-01 article");
    ledger.at("references").as_array().push_back(value);
  }
  std::unordered_set<std::string> move_ids;
  auto& reviewed_cards = ledger.at("cards").as_array();
  for (const auto& value : move_cards) {
    const auto& record = value.as_object();
    const std::string id(record.at("question_id").as_string());
    const bool is_named_question = id == "move_named_rvalue_reference_is_lvalue";
    const bool is_noexcept_question = id == "move_if_noexcept_copy_fallback";
    const bool single_binding = record.at("reference_ids").as_array().size() == 1;
    const bool valid_card = (is_named_question || is_noexcept_question) && move_ids.insert(id).second && single_binding;
    Check(valid_card, "MOVE-01 review contains unexpected or repeated question");
    const auto old = std::ranges::find_if(
        reviewed_cards, [&](const auto& entry) { return entry.as_object().at("question_id").as_string() == id; });
    Check(old == reviewed_cards.end(), "MOVE-01 overwrote historical reference matrix");
    reviewed_cards.push_back(value);
  }
  Check(move_articles.size() == 2 && move_ids.size() == 2, "MOVE-01 reviewed scope incomplete");

  // Preserve historical review entries; apply only six pinned successor decisions.
  std::ifstream quality_input("docs/reviews/references/2026-11-24-quality-fixes.json");
  Check(quality_input.is_open(), "missing question quality review");
  const std::string quality_text{std::istreambuf_iterator<char>(quality_input), std::istreambuf_iterator<char>()};
  const auto quality = boost::json::parse(quality_text).as_object();
  const auto& quality_reviewer = quality.at("reviewer").as_object();
  const bool quality_schema = quality.at("schema_version") == 1;
  const bool quality_standard = quality.at("standard_document") == "N4950";
  const bool quality_self_review = quality_reviewer.at("mode") == "self_review";
  const bool quality_independent = quality_reviewer.at("independent").as_bool();
  Check(quality_schema && quality_standard && quality_self_review && !quality_independent,
        "invalid question quality review provenance");
  const auto& quality_refs = quality.at("references").as_array();
  const auto& quality_cards = quality.at("cards").as_array();
  Check(quality_refs.size() == 3 && quality_cards.size() == 6, "unexpected quality review scope");
  const std::unordered_set<std::string> expected_quality_refs{
      "qa20261124.algorithm_constraints", "qa20261124.automatic_scope_check", "qa20261124.map_probe_contract"};
  std::unordered_set<std::string> actual_quality_refs;
  for (const auto& entry : quality_refs) {
    const std::string id(entry.as_object().at("id").as_string());
    Check(expected_quality_refs.contains(id) && actual_quality_refs.insert(id).second,
          "unexpected or repeated quality reference");
    ledger.at("references").as_array().push_back(entry);
  }
  std::unordered_set<std::string> allowed_quality_ids;
  allowed_quality_ids.insert("ranges_sort_list_constraints");
  allowed_quality_ids.insert("vector_raii_early_return");
  allowed_quality_ids.insert("map_transparent_lookup");
  allowed_quality_ids.insert("weak_ptr_lock_is_atomic_access");
  allowed_quality_ids.insert("ranges_search_local_owner_return");
  allowed_quality_ids.insert("vector_append_exception_contract");
  std::unordered_set<std::string> quality_ids;
  for (const auto& entry : quality_cards) {
    const auto& record = entry.as_object();
    const std::string id(record.at("question_id").as_string());
    Check(allowed_quality_ids.contains(id) && quality_ids.insert(id).second, "unexpected quality review card");
    auto& current_cards = ledger.at("cards").as_array();
    const auto expected_question_id = record.at("question_id");
    const auto same_question = [&](const auto& item) {
      return item.as_object().at("question_id") == expected_question_id;
    };
    const auto old = std::ranges::find_if(current_cards, same_question);
    Check(old != current_cards.end(), "quality review card is missing from history");
    const auto& previous = old->as_object();
    Check(previous.at("content_revision") == record.at("previous_content_revision") &&
              previous.at("reference_ids") == record.at("previous_reference_ids") &&
              previous.at("public_id") == record.at("public_id"),
          "quality review predecessor differs from reviewed history");
    *old = boost::json::object{{"question_id", record.at("question_id")},
                               {"public_id", record.at("public_id")},
                               {"content_revision", record.at("content_revision")},
                               {"reference_ids", record.at("reference_ids")},
                               {"rationale", record.at("rationale")}};
  }
  Check(actual_quality_refs.size() == 3 && quality_ids.size() == 6, "quality review scope incomplete");
  // Apply the five exact vector QA successors without changing historical ledgers.
  std::ifstream vector_input("docs/reviews/references/2026-11-24-vector-qa-batch01.json");
  Check(vector_input.is_open(), "missing vector QA reference review");
  const std::string vector_text{std::istreambuf_iterator<char>(vector_input), std::istreambuf_iterator<char>()};
  const auto vector_review = boost::json::parse(vector_text).as_object();
  Check(vector_review.at("schema_version") == 1 && vector_review.at("standard_document") == "N4950" &&
            vector_review.at("reviewer").as_object().at("mode") == "self_review" &&
            !vector_review.at("reviewer").as_object().at("independent").as_bool(),
        "invalid vector QA review provenance");
  const std::unordered_set<std::string> expected_vector_ids{
      "erase_remove_vector_size", "vector_access_complexity_matching", "vector_invalidation_multiple_choice",
      "vector_erase_filter_loop", "vector_reserve_bug_line"};
  std::unordered_set<std::string> actual_vector_ids;
  Check(vector_review.at("references").as_array().empty(), "vector QA must not change existing articles");
  for (const auto& entry : vector_review.at("cards").as_array()) {
    const auto& record = entry.as_object();
    const std::string id(record.at("question_id").as_string());
    Check(expected_vector_ids.contains(id) && actual_vector_ids.insert(id).second, "unexpected vector QA card");
    auto& current_cards = ledger.at("cards").as_array();
    const auto old = std::ranges::find_if(current_cards, [&](const auto& item) {
      return item.as_object().at("question_id") == record.at("question_id");
    });
    Check(old != current_cards.end(), "missing vector QA predecessor");
    const auto& previous = old->as_object();
    Check(previous.at("content_revision") == record.at("previous_content_revision") &&
              previous.at("reference_ids") == record.at("previous_reference_ids") &&
              previous.at("public_id") == record.at("public_id") &&
              record.at("reference_ids") == record.at("previous_reference_ids"),
          "vector QA predecessor or unchanged reference differs from reviewed history");
    *old = boost::json::object{{"question_id", record.at("question_id")},
                               {"public_id", record.at("public_id")},
                               {"content_revision", record.at("content_revision")},
                               {"reference_ids", record.at("reference_ids")},
                               {"rationale", record.at("rationale")}};
  }
  Check(actual_vector_ids == expected_vector_ids, "vector QA review scope incomplete");
  // Exact successor mappings for map QA; the original articles are unchanged.
  std::ifstream map_input("docs/reviews/references/2026-11-24-map-qa-batch02.json");
  Check(map_input.is_open(), "missing map QA reference review");
  const std::string map_text{std::istreambuf_iterator<char>(map_input), std::istreambuf_iterator<char>()};
  const auto map_review = boost::json::parse(map_text).as_object();
  Check(map_review.at("schema_version") == 1 && map_review.at("standard_document") == "N4950" &&
            map_review.at("reviewer").as_object().at("mode") == "self_review" &&
            !map_review.at("reviewer").as_object().at("independent").as_bool(),
        "invalid map QA review provenance");
  const auto& map_refs = map_review.at("references").as_array();
  const auto& map_cards = map_review.at("cards").as_array();
  Check(map_refs.size() == 2 && map_cards.size() == 11, "unexpected map QA scope");
  const std::unordered_set<std::string> expected_map_refs{"qa20261124.map_lookup_checklist",
                                                          "qa20261124.map_mutation_checklist"};
  std::unordered_set<std::string> map_ref_ids;
  for (const auto& entry : map_refs) {
    const std::string id(entry.as_object().at("id").as_string());
    Check(expected_map_refs.contains(id) && map_ref_ids.insert(id).second, "unexpected map QA article");
    ledger.at("references").as_array().push_back(entry);
  }
  const std::unordered_set<std::string> expected_map_ids{
      "map_find_iterator_completion", "map_contains_completion",
      "map_try_emplace_completion", "map_insert_or_assign_completion",
      "review_cache_read_contract", "review_mark_existing_contract",
      "map_lookup_methods_matching", "map_subscript_inserts_default_value",
      "map_find_without_insertion", "map_insert_or_assign_vs_try_emplace",
      "map_update_existing_stock"};
  std::unordered_set<std::string> map_ids;
  for (const auto& entry : map_cards) {
    const auto& record = entry.as_object();
    const std::string id(record.at("question_id").as_string());
    Check(expected_map_ids.contains(id) && map_ids.insert(id).second, "unexpected map QA card");
    auto& cards = ledger.at("cards").as_array();
    const auto same_question = [&](const auto& item) {
      return item.as_object().at("question_id") == record.at("question_id");
    };
    const auto old = std::ranges::find_if(cards, same_question);
    Check(old != cards.end(), "missing map QA predecessor");
    const auto& previous = old->as_object();
    Check(previous.at("content_revision") == record.at("previous_content_revision") &&
              previous.at("reference_ids") == record.at("previous_reference_ids") &&
              previous.at("public_id") == record.at("public_id"),
          "map QA predecessor differs from reviewed history");
    const auto& selected = record.at("reference_ids").as_array();
    Check(selected.size() == 1 && expected_map_refs.contains(std::string(selected.front().as_string())),
          "map QA restored a broad answer table");
    *old = boost::json::object{{"question_id", record.at("question_id")},
                               {"public_id", record.at("public_id")},
                               {"content_revision", record.at("content_revision")},
                               {"reference_ids", record.at("reference_ids")},
                               {"rationale", record.at("rationale")}};
  }
  Check(map_ref_ids == expected_map_refs && map_ids == expected_map_ids, "map QA scope incomplete");
  // Exact unordered QA successors. Earlier reviews and articles stay immutable.
  std::ifstream unordered_input("docs/reviews/references/2026-11-24-unordered-qa-batch03.json");
  Check(unordered_input.is_open(), "missing unordered QA reference review");
  const std::string unordered_text{std::istreambuf_iterator<char>(unordered_input), std::istreambuf_iterator<char>()};
  const auto unordered_review = boost::json::parse(unordered_text).as_object();
  Check(unordered_review.at("schema_version") == 1 && unordered_review.at("standard_document") == "N4950" &&
            unordered_review.at("reviewer").as_object().at("mode") == "self_review" &&
            !unordered_review.at("reviewer").as_object().at("independent").as_bool(),
        "invalid unordered QA review provenance");
  const std::unordered_set<std::string> expected_unordered_refs{
      "qa20261124.unordered_member_path", "qa20261124.unordered_handle_check", "qa20261124.unordered_commit_check"};
  std::unordered_set<std::string> unordered_refs;
  for (const auto& entry : unordered_review.at("references").as_array()) {
    const std::string id(entry.as_object().at("id").as_string());
    Check(expected_unordered_refs.contains(id) && unordered_refs.insert(id).second, "unexpected unordered QA article");
    ledger.at("references").as_array().push_back(entry);
  }
  const std::unordered_set<std::string> expected_unordered_ids{
      "unordered_hash_equality_contract", "unordered_map_iterator_member_address_completion",
      "unordered_map_reserve_vs_rehash", "unordered_pointer_rehash_erase_review",
      "lru_insert_failure_atomicity_review"};
  std::unordered_set<std::string> unordered_ids;
  for (const auto& entry : unordered_review.at("cards").as_array()) {
    const auto& record = entry.as_object();
    const std::string id(record.at("question_id").as_string());
    Check(expected_unordered_ids.contains(id) && unordered_ids.insert(id).second, "unexpected unordered QA card");
    auto& cards = ledger.at("cards").as_array();
    const auto same_question = [&](const auto& item) {
      return item.as_object().at("question_id") == record.at("question_id");
    };
    const auto old = std::ranges::find_if(cards, same_question);
    Check(old != cards.end(), "missing unordered QA predecessor");
    const auto& previous = old->as_object();
    Check(previous.at("content_revision") == record.at("previous_content_revision") &&
              previous.at("reference_ids") == record.at("previous_reference_ids") &&
              previous.at("public_id") == record.at("public_id"),
          "unordered QA predecessor differs from reviewed history");
    *old = boost::json::object{{"question_id", record.at("question_id")},
                               {"public_id", record.at("public_id")},
                               {"content_revision", record.at("content_revision")},
                               {"reference_ids", record.at("reference_ids")},
                               {"rationale", record.at("rationale")}};
  }
  Check(unordered_refs == expected_unordered_refs && unordered_ids == expected_unordered_ids,
        "unordered QA scope incomplete");
  ValidateLedger(ledger, questions);

  // Only the intended MOVE-01 card receives each preparation article.
  const auto named_move = std::ranges::find(questions, "move_named_rvalue_reference_is_lvalue", &Question::id);
  const auto noexcept_move = std::ranges::find(questions, "move_if_noexcept_copy_fallback", &Question::id);
  Check(named_move != questions.end() && noexcept_move != questions.end(), "MOVE-01 cards missing");
  const auto named_help = ReferencesForQuestion(*named_move);
  const auto noexcept_help = ReferencesForQuestion(*noexcept_move);
  const bool correct_named_help = named_help.size() == 1 && named_help[0]->id == "move01.named_reference_category";
  const bool correct_noexcept_help = noexcept_help.size() == 1 && noexcept_help[0]->id == "move01.noexcept_selection";
  Check(correct_named_help && correct_noexcept_help, "MOVE-01 preparation missing or attached to wrong card");
  for (const auto& question : questions) {
    if (question.id == named_move->id || question.id == noexcept_move->id) {
      continue;
    }
    for (const auto* entry : ReferencesForQuestion(question)) {
      Check(entry->id != "move01.named_reference_category", "MOVE-01 named reference help leaked");
      Check(entry->id != "move01.noexcept_selection", "MOVE-01 noexcept help leaked");
    }
  }
  const auto forwarding = std::ranges::find(questions, "forwarding_reference_lvalue_deduction", &Question::id);
  Check(forwarding != questions.end(), "missing forwarding card");
  const auto introduction = ReferencesForQuestion(*forwarding);
  Check(introduction.size() == 1 && introduction.front()->id == "forwarding_reference_basics",
        "forwarding card has no specific before-answer introduction");
  for (const auto& question : questions) {
    if (question.skill_id == "templates.deduction" && question.id != forwarding->id) {
      const auto references = ReferencesForQuestion(question);
      Check(std::ranges::none_of(references,
                                 [](const auto* entry) { return entry->id == "forwarding_reference_basics"; }),
            "forwarding introduction leaked to a different deduction mechanism");
    }
  }

  std::size_t bound_cards = 0;
  for (const auto& question : questions) {
    const auto expected = ReferencesForQuestion(question);
    bound_cards += !expected.empty();
    auto changed = question;
    changed.tags = {"vector", "map", "lifetime", "invoke_result", "span", "fold_expression"};
    Check(ReferencesForQuestion(changed) == expected, "tag overlap changed reviewed reference selection");
    changed.content_revision = "not-reviewed";
    Check(ReferencesForQuestion(changed).empty(), "stale card inherited references");
    changed = question;
    changed.id = "unknown-" + question.id;
    Check(ReferencesForQuestion(changed).empty(), "an unknown card inherited references");
  }
  Check(bound_cards == ReferenceBindingCatalog().size(), "unused or duplicate compiled binding");

  for (const bool duplicate : {false, true}) {
    auto changed = ledger;
    auto& cards = changed.at("cards").as_array();
    if (duplicate) {
      cards.push_back(boost::json::value(cards.front()));
    } else {
      cards.pop_back();
    }
    MustReject([&] { ValidateLedger(changed, questions); });
  }
  for (const auto field : {"content_revision", "public_id", "question_id", "rationale"}) {
    auto changed = ledger;
    changed.at("cards").as_array().front().as_object()[field] = "";
    MustReject([&] { ValidateLedger(changed, questions); });
  }
  for (const auto replacement : {"vector_storage", "missing-reference"}) {
    auto changed = ledger;
    auto& cards = changed.at("cards").as_array();
    auto& slicing = std::ranges::find_if(cards, [](const auto& value) {
                      return value.as_object().at("question_id").as_string() == "oop_vector_base_slices_derived";
                    })->as_object();
    slicing["reference_ids"] = boost::json::array{replacement};
    MustReject([&] { ValidateLedger(changed, questions); });
  }
  auto stale_article = ledger;
  stale_article.at("references").as_array().front().as_object()["content_revision"] = "not-reviewed";
  MustReject([&] { ValidateLedger(stale_article, questions); });
  auto changed_entry = ReferenceCatalog().front();
  Check(changed_entry.source_kind == LearningSourceKind::kDesignPrinciple,
        "design reference fixture is no longer first");
  auto language = changed_entry;
  language.source_kind = LearningSourceKind::kCppStandard;
  Check(ReferenceContentRevision(changed_entry) != ReferenceContentRevision(language),
        "reference hash ignores its source domain");
  const auto design_url = changed_entry.sources.front();
  Check(IsLearningSource(design_url, LearningSourceKind::kDesignPrinciple) &&
            !IsLearningSource(design_url, LearningSourceKind::kCppStandard) &&
            !IsLearningSource("https://example.com/solid", LearningSourceKind::kDesignPrinciple) &&
            !IsLearningSource("https://blog.cleancoder.com.attacker.invalid/", LearningSourceKind::kDesignPrinciple),
        "primary source validation mixed subjects or accepted an arbitrary host");
  changed_entry.mechanism = "changed after review";
  Check(ReferenceContentRevision(changed_entry) != ReferenceContentRevision(ReferenceCatalog().front()),
        "article hash ignores explanatory text");

  auto stale_card = questions;
  const auto bindings = ReferenceBindingCatalog();
  const auto first_id = bindings.front().question_id;
  std::ranges::find(stale_card, first_id, &Question::id)->content_revision = "not-reviewed";
  MustReject([&] { ValidateReferenceCatalog(stale_card); });
  auto missing_card = questions;
  std::erase_if(missing_card, [first_id](const Question& question) { return question.id == first_id; });
  MustReject([&] { ValidateReferenceCatalog(missing_card); });
}
