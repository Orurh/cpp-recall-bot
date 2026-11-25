#include "learning/content_source.h"
#include "quiz/question_identity.h"

#include <boost/json.hpp>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace {

using Bank = std::unordered_map<std::string, boost::json::object>;
using ReviewedSuccessors = std::unordered_map<std::string, std::pair<std::string, std::string>>;

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
void ValidateReviews(const boost::json::object& document, const Bank& bank,
                     const ReviewedSuccessors* successors = nullptr,
                     const ReviewedSuccessors* audit_successors = nullptr) {
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
    const auto matches_current = [&](const std::string& reviewed) {
      if (revision == reviewed)
        return true;
      if (audit_successors == nullptr)
        return false;
      const auto audit = audit_successors->find(id);
      return audit != audit_successors->end() && audit->second.first == reviewed && audit->second.second == revision;
    };
    bool reviewed_revision = matches_current(std::string(record.at("content_revision").as_string()));
    if (!reviewed_revision && successors != nullptr) {
      const auto next = successors->find(id);
      if (next != successors->end()) {
        const bool matches_previous = record.at("content_revision").as_string() == next->second.first;
        reviewed_revision = matches_previous && matches_current(next->second.second);
      }
    }
    Check(reviewed_revision, "card changed: repeat the technical review of " + id);
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

  const auto audit = ReadObject("docs/reviews/content/2026-11-26-audit-distinguishing.json");
  ValidateReviews(audit, bank);
  const auto& audit_old = audit.at("replaces_card_revisions").as_object();
  const auto& audit_records = audit.at("reviews").as_array();
  Check(audit_old.size() == 8 && audit_records.size() == 8, "unexpected audit scope");
  ReviewedSuccessors audit_successors;
  for (const auto& entry : audit_records) {
    const auto& record = entry.as_object();
    const std::string id(record.at("question_id").as_string());
    const std::string previous(audit_old.at(id).as_string());
    const std::string current(record.at("content_revision").as_string());
    Check(previous != current && audit_successors.emplace(id, std::make_pair(previous, current)).second,
          "invalid audit successor");
    std::size_t matches = 0;
    const char* historical_paths[] = {
        "docs/reviews/content/2026-11-25-lifetime-references-qa-batch08.json",
        "docs/reviews/content/2026-11-25-overload-qa-batch14.json",
        "docs/reviews/content/2026-11-25-stl-qa-batch04.json",
        "docs/reviews/content/2026-11-25-move-copy-qa-batch11.json",
        "docs/reviews/content/2026-10-26-map-methods.json",
    };
    for (const auto* path : historical_paths) {
      const auto history = ReadObject(path);
      for (const auto& item : history.at("reviews").as_array()) {
        const auto& old = item.as_object();
        if (old.at("question_id") == record.at("question_id")) {
          Check(old.at("content_revision").as_string() == previous, "audit predecessor differs from history");
          ++matches;
        }
      }
    }
    Check(matches == 1, "missing or duplicate audit predecessor");
  }
  const auto validate = [&](const boost::json::object& review, const Bank& cards,
                            const ReviewedSuccessors* successors = nullptr) {
    ValidateReviews(review, cards, successors, &audit_successors);
  };
  const auto views_qa = ReadObject("docs/reviews/content/2026-11-25-views-qa-batch06.json");
  validate(views_qa, bank);
  const auto& views_old = views_qa.at("replaces_card_revisions").as_object();
  const auto& views_records = views_qa.at("reviews").as_array();
  Check(views_old.size() == 7 && views_records.size() == 7, "unexpected views QA scope");
  ReviewedSuccessors views_successors;
  const auto is_ranges_review = [](const auto& value) {
    return value.as_object().at("question_id").as_string() == "ranges_search_local_owner_return";
  };
  const auto second_revision = std::ranges::find_if(views_records, is_ranges_review);
  Check(second_revision != views_records.end(), "missing second ranges review");
  const std::string ranges_previous(views_old.at("ranges_search_local_owner_return").as_string());
  const std::string ranges_current(second_revision->as_object().at("content_revision").as_string());
  views_successors.emplace("ranges_search_local_owner_return", std::make_pair(ranges_previous, ranges_current));
  const auto quality = ReadObject("docs/reviews/content/2026-11-24-quality-fixes.json");
  validate(quality, bank, &views_successors);
  const auto& old_revisions = quality.at("replaces_card_revisions").as_object();
  ReviewedSuccessors successors;
  for (const auto& record : quality.at("reviews").as_array()) {
    const auto& card = record.as_object();
    const std::string id(card.at("question_id").as_string());
    const auto& old = old_revisions.at(id).as_string();
    const auto& next = card.at("content_revision").as_string();
    Check(successors.emplace(id, std::make_pair(std::string(old), std::string(next))).second,
          "duplicate content successor");
  }
  Check(successors.size() == 3 && old_revisions.size() == 3, "unexpected content successor scope");
  const auto vector_qa = ReadObject("docs/reviews/content/2026-11-24-vector-qa-batch01.json");
  validate(vector_qa, bank);
  const auto& vector_old = vector_qa.at("replaces_card_revisions").as_object();
  Check(vector_old.size() == 5 && vector_qa.at("reviews").as_array().size() == 5, "unexpected vector QA review scope");
  for (const auto& entry : vector_qa.at("reviews").as_array()) {
    const auto& record = entry.as_object();
    const std::string id(record.at("question_id").as_string());
    const std::string previous(vector_old.at(id).as_string());
    const std::string current(record.at("content_revision").as_string());
    Check(previous != current && successors.emplace(id, std::make_pair(previous, current)).second,
          "duplicate or unchanged vector QA successor");
    std::size_t matching_predecessors = 0;
    const char* historical_paths[] = {
        "docs/reviews/content/2026-10-25-batch05-remaining-60.json",
        "docs/reviews/content/2026-10-26-container-basics.json",
        "docs/reviews/content/2026-11-06-mobile-bug.json",
    };
    for (const auto* path : historical_paths) {
      const auto historical = ReadObject(path);
      for (const auto& old_entry : historical.at("reviews").as_array()) {
        const auto& old_record = old_entry.as_object();
        if (old_record.at("question_id").as_string() == id) {
          Check(old_record.at("content_revision").as_string() == previous,
                "vector QA predecessor differs from reviewed history");
          ++matching_predecessors;
        }
      }
    }
    Check(matching_predecessors == 1, "missing or duplicate historical vector QA review");
  }
  const auto map_qa = ReadObject("docs/reviews/content/2026-11-24-map-qa-batch02.json");
  validate(map_qa, bank);
  const auto& map_old = map_qa.at("replaces_card_revisions").as_object();
  const bool map_scope = map_old.size() == 7 && map_qa.at("reviews").as_array().size() == 7;
  Check(map_scope, "unexpected map QA review scope");
  for (const auto& entry : map_qa.at("reviews").as_array()) {
    const auto& record = entry.as_object();
    const std::string id(record.at("question_id").as_string());
    const std::string previous(map_old.at(id).as_string());
    const std::string current(record.at("content_revision").as_string());
    Check(previous != current && successors.emplace(id, std::make_pair(previous, current)).second,
          "duplicate or unchanged map QA successor");
    std::size_t predecessors = 0;
    const char* historical_paths[] = {
        "docs/reviews/content/2026-10-26-map-methods.json",
        "docs/reviews/content/2026-10-28-review-api-pilot.json",
    };
    for (const auto* path : historical_paths) {
      const auto historical = ReadObject(path);
      for (const auto& item : historical.at("reviews").as_array()) {
        const auto& before = item.as_object();
        if (before.at("question_id").as_string() == id) {
          Check(before.at("content_revision").as_string() == previous, "map QA predecessor changed");
          ++predecessors;
        }
      }
    }
    Check(predecessors == 1, "missing or duplicate map QA predecessor");
  }
  const auto unordered_qa = ReadObject("docs/reviews/content/2026-11-24-unordered-qa-batch03.json");
  validate(unordered_qa, bank);
  const auto& unordered_old = unordered_qa.at("replaces_card_revisions").as_object();
  const bool unordered_scope = unordered_old.size() == 5 && unordered_qa.at("reviews").as_array().size() == 5;
  Check(unordered_scope, "unexpected unordered QA review scope");
  for (const auto& entry : unordered_qa.at("reviews").as_array()) {
    const auto& record = entry.as_object();
    const std::string id(record.at("question_id").as_string());
    const std::string previous(unordered_old.at(id).as_string());
    const std::string current(record.at("content_revision").as_string());
    Check(previous != current && successors.emplace(id, std::make_pair(previous, current)).second,
          "duplicate or unchanged unordered QA successor");
    std::size_t predecessors = 0;
    const char* historical_paths[] = {
        "docs/reviews/content/2026-10-25-batch05-remaining-60.json",
        "docs/reviews/content/2026-11-23-content02-containers.json",
    };
    for (const auto* path : historical_paths) {
      const auto historical = ReadObject(path);
      for (const auto& item : historical.at("reviews").as_array()) {
        const auto& before = item.as_object();
        if (before.at("question_id").as_string() == id) {
          Check(before.at("content_revision").as_string() == previous, "unordered QA predecessor changed");
          ++predecessors;
        }
      }
    }
    Check(predecessors == 1, "missing or duplicate unordered QA predecessor");
  }
  const auto stl_qa = ReadObject("docs/reviews/content/2026-11-25-stl-qa-batch04.json");
  validate(stl_qa, bank);
  const auto& stl_old = stl_qa.at("replaces_card_revisions").as_object();
  Check(stl_old.size() == 6 && stl_qa.at("reviews").as_array().size() == 6, "unexpected STL QA scope");
  for (const auto& entry : stl_qa.at("reviews").as_array()) {
    const auto& record = entry.as_object();
    const std::string id(record.at("question_id").as_string());
    const std::string previous(stl_old.at(id).as_string());
    const std::string current(record.at("content_revision").as_string());
    Check(previous != current && successors.emplace(id, std::make_pair(previous, current)).second,
          "duplicate or unchanged STL QA successor");
    std::size_t predecessors = 0;
    const char* historical_paths[] = {
        "docs/reviews/content/2026-10-25-batch05-remaining-60.json",
        "docs/reviews/content/2026-10-26-snapshot-library.json",
        "docs/reviews/content/2026-11-23-content02-containers.json",
    };
    for (const auto* path : historical_paths) {
      const auto historical = ReadObject(path);
      for (const auto& item : historical.at("reviews").as_array()) {
        const auto& before = item.as_object();
        if (before.at("question_id").as_string() == id) {
          Check(before.at("content_revision").as_string() == previous, "STL QA predecessor changed");
          ++predecessors;
        }
      }
    }
    Check(predecessors == 1, "missing or duplicate STL QA predecessor");
  }
  const auto composed_qa = ReadObject("docs/reviews/content/2026-11-25-composed-qa-batch05.json");
  validate(composed_qa, bank);
  const auto& composed_old = composed_qa.at("replaces_card_revisions").as_object();
  const auto& composed_records = composed_qa.at("reviews").as_array();
  Check(composed_old.size() == 4 && composed_records.size() == 4, "unexpected composed QA scope");
  const auto composed_history = ReadObject("docs/reviews/content/2026-11-23-content02-containers.json");
  for (const auto& entry : composed_records) {
    const auto& record = entry.as_object();
    const std::string id(record.at("question_id").as_string());
    const std::string previous(composed_old.at(id).as_string());
    const std::string current(record.at("content_revision").as_string());
    Check(previous != current && successors.emplace(id, std::make_pair(previous, current)).second,
          "duplicate or unchanged composed QA successor");
    const auto& history = composed_history.at("reviews").as_array();
    const auto same_question = [&](const auto& item) {
      return item.as_object().at("question_id") == record.at("question_id");
    };
    const auto old = std::ranges::find_if(history, same_question);
    Check(old != history.end(), "missing composed QA predecessor");
    Check(old->as_object().at("content_revision").as_string() == previous, "composed QA predecessor changed");
  }
  for (const auto& entry : views_records) {
    const auto& record = entry.as_object();
    const std::string id(record.at("question_id").as_string());
    const std::string previous(views_old.at(id).as_string());
    const std::string current(record.at("content_revision").as_string());
    Check(previous != current, "unchanged views QA revision");
    if (id == "ranges_search_local_owner_return") {
      const auto prior = successors.find(id);
      Check(prior != successors.end() && prior->second.second == previous, "broken ranges review chain");
      prior->second.second = current;
      continue;
    }
    Check(successors.emplace(id, std::make_pair(previous, current)).second, "duplicate views QA successor");
    std::size_t predecessors = 0;
    const char* historical_paths[] = {
        "docs/reviews/content/2026-10-25-high-risk.json",
        "docs/reviews/content/2026-10-25-batch05-remaining-60.json",
        "docs/reviews/content/2026-10-26-snapshot-library.json",
        "docs/reviews/content/2026-11-23-content02-containers.json",
    };
    for (const auto* path : historical_paths) {
      const auto historical = ReadObject(path);
      for (const auto& item : historical.at("reviews").as_array()) {
        const auto& before = item.as_object();
        if (before.at("question_id").as_string() == id) {
          Check(before.at("content_revision").as_string() == previous, "views QA predecessor changed");
          ++predecessors;
        }
      }
    }
    Check(predecessors == 1, "missing or duplicate views QA predecessor");
  }
  const auto final_qa = ReadObject("docs/reviews/content/2026-11-25-container-final-qa-batch07.json");
  validate(final_qa, bank);
  const auto& final_old = final_qa.at("replaces_card_revisions").as_object();
  const auto& final_records = final_qa.at("reviews").as_array();
  Check(final_old.size() == 6 && final_records.size() == 6, "unexpected final container QA scope");
  for (const auto& entry : final_records) {
    const auto& record = entry.as_object();
    const std::string id(record.at("question_id").as_string());
    const std::string previous(final_old.at(id).as_string());
    const std::string current(record.at("content_revision").as_string());
    Check(previous != current && successors.emplace(id, std::make_pair(previous, current)).second,
          "duplicate or unchanged final container QA successor");
    std::size_t predecessors = 0;
    const char* historical_paths[] = {
        "docs/reviews/content/2026-10-25-batch05-remaining-60.json",
        "docs/reviews/content/2026-10-25-high-risk.json",
        "docs/reviews/content/2026-11-23-content02-containers.json",
    };
    for (const auto* path : historical_paths) {
      const auto historical = ReadObject(path);
      for (const auto& item : historical.at("reviews").as_array()) {
        const auto& before = item.as_object();
        if (before.at("question_id").as_string() == id) {
          Check(before.at("content_revision").as_string() == previous, "final container QA predecessor changed");
          ++predecessors;
        }
      }
    }
    Check(predecessors == 1, "missing or duplicate final container QA predecessor");
  }
  const auto lifetime_qa = ReadObject("docs/reviews/content/2026-11-25-lifetime-references-qa-batch08.json");
  validate(lifetime_qa, bank);
  const auto& lifetime_old = lifetime_qa.at("replaces_card_revisions").as_object();
  const auto& lifetime_records = lifetime_qa.at("reviews").as_array();
  Check(lifetime_old.size() == 14 && lifetime_records.size() == 14, "unexpected lifetime QA scope");
  for (const auto& entry : lifetime_records) {
    const auto& record = entry.as_object();
    const std::string id(record.at("question_id").as_string());
    const std::string previous(lifetime_old.at(id).as_string());
    const std::string current(record.at("content_revision").as_string());
    Check(previous != current && successors.emplace(id, std::make_pair(previous, current)).second,
          "duplicate or unchanged lifetime QA successor");
    std::size_t predecessors = 0;
    const char* historical_paths[] = {
        "docs/reviews/content/2026-10-25-high-risk.json",
        "docs/reviews/content/2026-10-25-batch05-remaining-60.json",
        "docs/reviews/content/2026-10-28-values-references.json",
    };
    for (const auto* path : historical_paths) {
      const auto historical = ReadObject(path);
      for (const auto& item : historical.at("reviews").as_array()) {
        const auto& before = item.as_object();
        if (before.at("question_id").as_string() == id) {
          Check(before.at("content_revision").as_string() == previous, "lifetime QA predecessor changed");
          ++predecessors;
        }
      }
    }
    Check(predecessors == 1, "missing or duplicate lifetime QA predecessor");
  }
  const auto ownership_qa = ReadObject("docs/reviews/content/2026-11-25-smart-pointers-qa-batch09.json");
  validate(ownership_qa, bank);
  const auto& ownership_old = ownership_qa.at("replaces_card_revisions").as_object();
  const auto& ownership_records = ownership_qa.at("reviews").as_array();
  Check(ownership_old.size() == 8 && ownership_records.size() == 8, "unexpected ownership QA scope");
  const auto ownership_history = ReadObject("docs/reviews/content/2026-10-25-high-risk.json");
  const auto& ownership_previous = ownership_history.at("reviews").as_array();
  for (const auto& entry : ownership_records) {
    const auto& record = entry.as_object();
    const std::string id(record.at("question_id").as_string());
    const std::string previous(ownership_old.at(id).as_string());
    const std::string current(record.at("content_revision").as_string());
    Check(previous != current && successors.emplace(id, std::make_pair(previous, current)).second,
          "duplicate or unchanged ownership QA successor");
    const auto same_question = [&](const auto& item) {
      return item.as_object().at("question_id") == record.at("question_id");
    };
    const auto old = std::ranges::find_if(ownership_previous, same_question);
    Check(old != ownership_previous.end(), "missing ownership QA predecessor");
    Check(old->as_object().at("content_revision").as_string() == previous, "ownership QA predecessor changed");
  }
  const auto raii_qa = ReadObject("docs/reviews/content/2026-11-25-raii-qa-batch10.json");
  validate(raii_qa, bank);
  const auto& raii_old = raii_qa.at("replaces_card_revisions").as_object();
  const auto& raii_records = raii_qa.at("reviews").as_array();
  Check(raii_old.size() == 8 && raii_records.size() == 8, "unexpected raii QA scope");
  const auto raii_history = ReadObject("docs/reviews/content/2026-10-25-batch05-remaining-60.json");
  const auto& raii_previous = raii_history.at("reviews").as_array();
  for (const auto& entry : raii_records) {
    const auto& record = entry.as_object();
    const std::string id(record.at("question_id").as_string());
    const std::string previous(raii_old.at(id).as_string());
    const std::string current(record.at("content_revision").as_string());
    Check(previous != current && successors.emplace(id, std::make_pair(previous, current)).second,
          "duplicate or unchanged raii QA successor");
    const auto same_question = [&](const auto& item) {
      return item.as_object().at("question_id") == record.at("question_id");
    };
    const auto old = std::ranges::find_if(raii_previous, same_question);
    Check(old != raii_previous.end(), "missing raii QA predecessor");
    Check(old->as_object().at("content_revision").as_string() == previous, "raii QA predecessor changed");
  }
  const auto move_qa = ReadObject("docs/reviews/content/2026-11-25-move-copy-qa-batch11.json");
  validate(move_qa, bank);
  const auto& move_old = move_qa.at("replaces_card_revisions").as_object();
  const auto& move_records = move_qa.at("reviews").as_array();
  Check(move_old.size() == 8 && move_records.size() == 8, "unexpected move/copy QA scope");
  for (const auto& entry : move_records) {
    const auto& record = entry.as_object();
    const std::string id(record.at("question_id").as_string());
    const std::string previous(move_old.at(id).as_string());
    const std::string current(record.at("content_revision").as_string());
    Check(previous != current && successors.emplace(id, std::make_pair(previous, current)).second,
          "duplicate or unchanged move/copy QA successor");
    std::size_t predecessors = 0;
    const char* historical_paths[] = {
        "docs/reviews/content/2026-10-25-batch02-templates-and-sources.json",
        "docs/reviews/content/2026-10-25-batch05-remaining-60.json",
        "docs/reviews/content/2026-10-28-values-references.json",
    };
    for (const auto* path : historical_paths) {
      const auto historical = ReadObject(path);
      for (const auto& item : historical.at("reviews").as_array()) {
        const auto& before = item.as_object();
        if (before.at("question_id").as_string() == id) {
          Check(before.at("content_revision").as_string() == previous, "move/copy QA predecessor changed");
          ++predecessors;
        }
      }
    }
    Check(predecessors == 1, "missing or duplicate move/copy QA predecessor");
  }
  const auto storage_qa = ReadObject("docs/reviews/content/2026-11-25-storage-qa-batch12.json");
  validate(storage_qa, bank);
  const auto& storage_old = storage_qa.at("replaces_card_revisions").as_object();
  const auto& storage_records = storage_qa.at("reviews").as_array();
  Check(storage_old.size() == 7 && storage_records.size() == 7, "unexpected storage QA scope");
  const auto storage_history = ReadObject("docs/reviews/content/2026-10-25-batch05-remaining-60.json");
  const auto& storage_previous = storage_history.at("reviews").as_array();
  for (const auto& entry : storage_records) {
    const auto& record = entry.as_object();
    const std::string id(record.at("question_id").as_string());
    const std::string previous(storage_old.at(id).as_string());
    const std::string current(record.at("content_revision").as_string());
    Check(previous != current && successors.emplace(id, std::make_pair(previous, current)).second,
          "duplicate or unchanged storage QA successor");
    const auto same_question = [&](const auto& item) {
      return item.as_object().at("question_id") == record.at("question_id");
    };
    const auto old = std::ranges::find_if(storage_previous, same_question);
    Check(old != storage_previous.end(), "missing storage QA predecessor");
    Check(old->as_object().at("content_revision").as_string() == previous, "storage QA predecessor changed");
  }
  const auto linkage_qa = ReadObject("docs/reviews/content/2026-11-25-linkage-qa-batch13.json");
  validate(linkage_qa, bank);
  const auto& linkage_old = linkage_qa.at("replaces_card_revisions").as_object();
  const auto& linkage_records = linkage_qa.at("reviews").as_array();
  Check(linkage_old.size() == 7 && linkage_records.size() == 7, "unexpected linkage QA scope");
  const auto linkage_history = ReadObject("docs/reviews/content/2026-10-25-batch05-remaining-60.json");
  const auto& linkage_previous = linkage_history.at("reviews").as_array();
  for (const auto& entry : linkage_records) {
    const auto& record = entry.as_object();
    const std::string id(record.at("question_id").as_string());
    const std::string previous(linkage_old.at(id).as_string());
    const std::string current(record.at("content_revision").as_string());
    Check(previous != current && successors.emplace(id, std::make_pair(previous, current)).second,
          "duplicate or unchanged linkage QA successor");
    const auto same_question = [&](const auto& item) {
      return item.as_object().at("question_id") == record.at("question_id");
    };
    const auto old = std::ranges::find_if(linkage_previous, same_question);
    Check(old != linkage_previous.end(), "missing linkage QA predecessor");
    Check(old->as_object().at("content_revision").as_string() == previous, "linkage QA predecessor changed");
  }
  const auto overload_qa = ReadObject("docs/reviews/content/2026-11-25-overload-qa-batch14.json");
  validate(overload_qa, bank);
  const auto& overload_old = overload_qa.at("replaces_card_revisions").as_object();
  const auto& overload_records = overload_qa.at("reviews").as_array();
  Check(overload_old.size() == 7 && overload_records.size() == 7, "unexpected overload QA scope");
  for (const auto& entry : overload_records) {
    const auto& record = entry.as_object();
    const std::string id(record.at("question_id").as_string());
    const std::string previous(overload_old.at(id).as_string());
    const std::string current(record.at("content_revision").as_string());
    Check(previous != current && successors.emplace(id, std::make_pair(previous, current)).second,
          "duplicate or unchanged overload QA successor");
    std::size_t predecessors = 0;
    const char* historical_paths[] = {
        "docs/reviews/content/2026-10-25-batch02-templates-and-sources.json",
        "docs/reviews/content/2026-10-25-batch03-oop-foundations.json",
        "docs/reviews/content/2026-10-25-batch04-oop-completion.json",
    };
    for (const auto* path : historical_paths) {
      const auto historical = ReadObject(path);
      for (const auto& item : historical.at("reviews").as_array()) {
        const auto& before = item.as_object();
        if (before.at("question_id").as_string() == id) {
          Check(before.at("content_revision").as_string() == previous, "overload QA predecessor changed");
          ++predecessors;
        }
      }
    }
    Check(predecessors == 1, "missing or duplicate overload QA predecessor");
  }
  // Validate each new batch against the exact immutable predecessor registry.
  const auto verify_batch = [&](const char* path, std::initializer_list<const char*> history_paths, std::size_t count) {
    const auto batch = ReadObject(path);
    validate(batch, bank);
    const auto& replacements = batch.at("replaces_card_revisions").as_object();
    const auto& records = batch.at("reviews").as_array();
    Check(records.size() == count && replacements.size() == count, "unexpected QA batch scope");
    for (const auto& entry : records) {
      const auto& record = entry.as_object();
      const std::string id(record.at("question_id").as_string());
      const std::string previous(replacements.at(id).as_string());
      const std::string current(record.at("content_revision").as_string());
      Check(previous != current && successors.emplace(id, std::make_pair(previous, current)).second,
            "duplicate or unchanged QA successor");
      std::size_t matches = 0;
      for (const auto* history_path : history_paths) {
        const auto history = ReadObject(history_path);
        for (const auto& old_entry : history.at("reviews").as_array()) {
          const auto& old = old_entry.as_object();
          if (old.at("question_id") == record.at("question_id")) {
            Check(old.at("content_revision").as_string() == previous, "QA predecessor changed");
            ++matches;
          }
        }
      }
      Check(matches == 1, "missing or duplicate QA predecessor");
    }
  };
  verify_batch("docs/reviews/content/2026-11-25-templates-qa-batch15.json",
               {"docs/reviews/content/2026-10-25-batch02-templates-and-sources.json"}, 8);
  verify_batch("docs/reviews/content/2026-11-25-packs-sfinae-qa-batch16.json",
               {"docs/reviews/content/2026-10-26-template-learning.json",
                "docs/reviews/content/2026-10-25-batch02-templates-and-sources.json"},
               5);
  verify_batch("docs/reviews/content/2026-11-25-concurrency-qa-batch17.json",
               {"docs/reviews/content/2026-10-25-high-risk.json"}, 9);
  verify_batch("docs/reviews/content/2026-11-26-virtual-functions-qa-batch18.json",
               {"docs/reviews/content/2026-10-25-batch03-oop-foundations.json",
                "docs/reviews/content/2026-10-25-batch04-oop-completion.json"},
               12);
  const auto foundations = "docs/reviews/content/2026-10-25-batch03-oop-foundations.json";
  const auto completion = "docs/reviews/content/2026-10-25-batch04-oop-completion.json";
  const auto move_review = "docs/reviews/content/2026-11-24-move01.json";
  const auto object_design = "docs/reviews/content/2026-11-26-object-design-qa-batch19.json";
  verify_batch(object_design, {foundations, completion, move_review}, 9);
  const auto document = ReadObject("docs/reviews/content/2026-10-25-high-risk.json");
  validate(document, bank, &successors);
  const auto design = ReadObject("docs/reviews/content/2026-10-27-solid.json");
  const auto sfinae = ReadObject("docs/reviews/content/2026-10-27-sfinae.json");
  validate(design, bank);
  validate(sfinae, bank);
  for (const auto* reviewed : {&design, &sfinae}) {
    auto fake_source = *reviewed;
    fake_source.at("reviews").as_array().front().as_object()["sources"] =
        boost::json::array{"https://example.com/unreviewed"};
    MustReject([&] { validate(fake_source, bank); });
    auto wrong_subject = *reviewed;
    wrong_subject["review_subject"] = reviewed == &design ? "cpp_language" : "software_design";
    MustReject([&] { validate(wrong_subject, bank); });
  }
  const auto& first = document.at("reviews").as_array().front().as_object();
  const std::string id(first.at("question_id").as_string());
  for (const auto field : {"text", "explanation", "code", "source"}) {
    auto changed = bank;
    changed.at(id)[field] = "changed after review";
    MustReject([&] { validate(document, changed, &successors); });
  }
  auto changed_key = bank;
  changed_key.at(id).at("task").as_object()["correct_option_id"] = "changed";
  MustReject([&] { validate(document, changed_key, &successors); });
  auto duplicate = document;
  duplicate.at("reviews").as_array().push_back(boost::json::value(first));
  MustReject([&] { validate(duplicate, bank, &successors); });
  for (const auto field : {"public_id", "skill_id", "question_family_id", "standard", "decision"}) {
    auto changed = document;
    changed.at("reviews").as_array().front().as_object()[field] = "changed";
    MustReject([&] { validate(changed, bank); });
  }
  auto no_sources = document;
  no_sources.at("reviews").as_array().front().as_object()["sources"] = boost::json::array{};
  MustReject([&] { validate(no_sources, bank, &successors); });
  std::cout << "content reviews: " << document.at("reviews").as_array().size() << '/' << bank.size()
            << " (source-based self-review; not independent acceptance of the full bank)\n";
}
