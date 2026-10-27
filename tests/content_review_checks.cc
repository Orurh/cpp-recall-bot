#include "learning/content_source.h"
#include "quiz/question_identity.h"

#include <boost/json.hpp>

#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>

namespace {

using Bank = std::unordered_map<std::string, boost::json::object>;

void Check(bool condition, std::string_view message) {
  if (!condition)
    throw std::runtime_error(std::string(message));
}

boost::json::object ReadObject(const std::filesystem::path& path) {
  std::ifstream input(path);
  Check(input.is_open(), "cannot open content review fixture");
  const std::string text{std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
  return boost::json::parse(text).as_object();
}

// This checks traceability, NOT the truth of an answer. Acceptance still requires source review.
void ValidateReviews(const boost::json::object& document, const Bank& bank) {
  Check(document.at("schema_version").as_int64() == 1, "unsupported content review schema");
  const auto* subject = document.if_contains("review_subject");
  const bool design = subject != nullptr && subject->as_string() == "software_design";
  Check(subject == nullptr || subject->as_string() == "cpp_language" || design, "unknown content review subject");
  Check(document.at("standard_document").as_string() == (design ? "Primary design sources" : "N4950"),
        "review must identify sources for its subject");
  const auto source_kind = design ? LearningSourceKind::kDesignPrinciple : LearningSourceKind::kCppStandard;
  Check(!document.at("reviewed_at").as_string().empty(), "review date missing");
  const auto& reviewer = document.at("reviewer").as_object();
  Check(!reviewer.at("name").as_string().empty(), "reviewer missing");
  const auto mode = reviewer.at("mode").as_string();
  Check(mode == "self_review" || mode == "independent_review", "review independence must be explicit");
  Check(reviewer.at("independent").as_bool() == (mode == "independent_review"), "contradictory review independence");
  const auto& records = document.at("reviews").as_array();
  Check(!records.empty(), "content review ledger must not be empty");
  std::unordered_set<std::string> seen;
  for (const auto& value : records) {
    const auto& record = value.as_object();
    const std::string id(record.at("question_id").as_string());
    Check(seen.emplace(id).second, "duplicate card in content review ledger");
    const auto found = bank.find(id);
    Check(found != bank.end(), "reviewed card is no longer in the bank");
    const auto& question = found->second;
    for (const auto field : {"public_id", "skill_id", "question_family_id", "standard"}) {
      Check(record.at(field) == question.at(field), "review metadata does not match the card");
    }
    Check(record.at("standard").as_string() == (design ? "Принципы проектирования" : "C++23"),
          "design principle was confused with a rule of the C++ standard");
    Check(record.at("decision").as_string() == "accepted", "card still needs a review decision");
    Check(!record.at("rationale").as_string().empty(), "review decision needs a rationale");
    const auto revision = QuestionContentRevision(CanonicalQuestionJson(question));
    Check(record.at("content_revision").as_string() == revision, "card changed: repeat the technical review of " + id);
    const auto& sources = record.at("sources").as_array();
    Check(!sources.empty(), "review must include normative sources");
    bool includes_card_source = false;
    for (const auto& source : sources) {
      const auto& url = source.as_string();
      Check(IsLearningSource(std::string_view(url.data(), url.size()), source_kind),
            "review source does not match its subject");
      includes_card_source = includes_card_source || source == question.at("source");
    }
    Check(includes_card_source, "card source missing from its review");
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
  Check(rejected, "stale or incomplete review was silently accepted");
}

}  // namespace

void CheckContentReviews() {
  Bank bank;
  for (const auto& file : std::filesystem::recursive_directory_iterator("content/questions")) {
    if (!file.is_regular_file() || file.path().extension() != ".json")
      continue;
    auto question = ReadObject(file.path());
    const std::string id(question.at("id").as_string());
    Check(bank.emplace(id, std::move(question)).second, "duplicate question id in review fixture");
  }
  const auto document = ReadObject("docs/reviews/content/2026-10-25-high-risk.json");
  ValidateReviews(document, bank);
  const auto design = ReadObject("docs/reviews/content/2026-10-27-solid.json");
  const auto sfinae = ReadObject("docs/reviews/content/2026-10-27-sfinae.json");
  ValidateReviews(design, bank);
  ValidateReviews(sfinae, bank);
  for (const auto* reviewed : {&design, &sfinae}) {
    auto fake_source = *reviewed;
    fake_source.at("reviews").as_array().front().as_object()["sources"] =
        boost::json::array{"https://example.com/unreviewed"};
    MustReject([&] { ValidateReviews(fake_source, bank); });
    auto wrong_subject = *reviewed;
    wrong_subject["review_subject"] = reviewed == &design ? "cpp_language" : "software_design";
    MustReject([&] { ValidateReviews(wrong_subject, bank); });
  }
  const auto& first = document.at("reviews").as_array().front().as_object();
  const std::string id(first.at("question_id").as_string());
  for (const auto field : {"text", "explanation", "code", "source"}) {
    auto changed = bank;
    changed.at(id)[field] = "changed after review";
    MustReject([&] { ValidateReviews(document, changed); });
  }
  auto changed_key = bank;
  changed_key.at(id).at("task").as_object()["correct_option_id"] = "changed";
  MustReject([&] { ValidateReviews(document, changed_key); });
  auto duplicate = document;
  duplicate.at("reviews").as_array().push_back(boost::json::value(first));
  MustReject([&] { ValidateReviews(duplicate, bank); });
  for (const auto field : {"public_id", "skill_id", "question_family_id", "standard", "decision"}) {
    auto changed = document;
    changed.at("reviews").as_array().front().as_object()[field] = "changed";
    MustReject([&] { ValidateReviews(changed, bank); });
  }
  auto no_sources = document;
  no_sources.at("reviews").as_array().front().as_object()["sources"] = boost::json::array{};
  MustReject([&] { ValidateReviews(no_sources, bank); });
  std::cout << "content reviews: " << document.at("reviews").as_array().size() << '/' << bank.size()
            << " (source-based self-review; not independent acceptance of the full bank)\n";
}
