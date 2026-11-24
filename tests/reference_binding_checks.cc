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
  Check(move_review.at("schema_version") == 1 && move_review.at("standard_document") == "N4950" &&
            move_review.at("reviewer").as_object().at("mode") == "self_review" &&
            !move_review.at("reviewer").as_object().at("independent").as_bool() &&
            move_review.at("references").as_array().size() == 2 &&
            move_review.at("cards").as_array().size() == 2,
        "unexpected MOVE-01 review provenance or scope");
  std::unordered_set<std::string> move_articles;
  for (const auto& value : move_review.at("references").as_array()) {
    const auto& entry = value.as_object();
    const std::string id(entry.at("id").as_string());
    Check((id == "move01.named_reference_category" || id == "move01.noexcept_selection") &&
              move_articles.insert(id).second, "unexpected or repeated MOVE-01 article");
    ledger.at("references").as_array().push_back(value);
  }
  std::unordered_set<std::string> move_ids;
  for (const auto& value : move_review.at("cards").as_array()) {
    const auto& record = value.as_object();
    const std::string id(record.at("question_id").as_string());
    Check((id == "move_named_rvalue_reference_is_lvalue" || id == "move_if_noexcept_copy_fallback") &&
              move_ids.insert(id).second && record.at("reference_ids").as_array().size() == 1,
          "MOVE-01 review contains unexpected or repeated question");
    const auto old = std::ranges::find_if(
        ledger.at("cards").as_array(), [&](const auto& entry) {
          return entry.as_object().at("question_id").as_string() == id;
        });
    Check(old == ledger.at("cards").as_array().end(), "MOVE-01 overwrote historical reference matrix");
    ledger.at("cards").as_array().push_back(value);
  }
  Check(move_articles.size() == 2 && move_ids.size() == 2, "MOVE-01 reviewed scope incomplete");
  ValidateLedger(ledger, questions);

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
