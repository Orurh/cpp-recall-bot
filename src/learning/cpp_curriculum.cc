#include "learning/cpp_curriculum.h"

#include "learning/cpp_curriculum_data.h"
#include "learning/reference_catalog.h"
#include "learning/skill_catalog.h"
#include "learning/theory_catalog.h"
#include "learning/training_recommendation.h"
#include "quiz/topic_catalog.h"

#include <boost/json.hpp>

#include <algorithm>
#include <map>
#include <set>
#include <stdexcept>

namespace {
const boost::json::object& Definition() {
  static const auto value = boost::json::parse(kCppCurriculumJson).as_object();
  return value;
}

std::string UnitId(std::string_view skill, int level) {
  return "cpp." + std::string(skill) + ".l" + std::to_string(level);
}

int WorkingLevel(const UserProgressSummary& progress, std::string_view topic) {
  const auto found = std::ranges::find(progress.topics, topic, &TopicProgressSummary::topic);
  return found == progress.topics.end() ? 3 : found->adaptive_level;
}

boost::json::array Strings(const std::set<std::string>& items) {
  boost::json::array result;
  for (const auto& item : items)
    result.emplace_back(item);
  return result;
}

bool EligibleQuestion(const Question& question, const UserProgressSummary& progress,
                      const std::unordered_map<std::string, QuestionReviewState>& reviews) {
  // A lowered working difficulty changes new material, not already-known review.
  return question.level <= progress.unlocked_level &&
         (reviews.contains(question.id) || (question.level <= WorkingLevel(progress, question.topic) &&
                                            CanIntroduceTrainingQuestion(question, progress)));
}

bool SafeUnit(const QuestionBank& bank, const UserProgressSummary& progress, const std::vector<std::string>& ids,
              const std::unordered_map<std::string, QuestionReviewState>& reviews) {
  return std::ranges::any_of(ids, [&](const auto& id) { return EligibleQuestion(bank.Get(id), progress, reviews); });
}
}  // namespace

void ValidateCppCurriculum(const QuestionBank& bank) {
  const auto& definition = Definition();
  if (definition.size() != 5 || definition.at("schema_version") != 1 || definition.at("direction") != "cpp" ||
      definition.at("pilot_topic") != "containers" || !definition.at("passing_rule").is_string())
    throw std::invalid_argument("Invalid C++ curriculum metadata");
  const auto& levels = definition.at("levels").as_array();
  if (levels.size() != 10)
    throw std::invalid_argument("C++ curriculum must describe levels 1–10");
  for (std::size_t index = 0; index < levels.size(); ++index) {
    const auto& level = levels[index].as_object();
    if (level.size() != 3 || level.at("level").as_int64() != static_cast<std::int64_t>(index + 1) ||
        level.at("title").as_string().empty() || level.at("goal").as_string().empty())
      throw std::invalid_argument("Invalid curriculum level goal");
  }
  for (const auto& id : bank.QuestionIds()) {
    const auto& question = bank.Get(id);
    const auto* skill = FindSkill(question.skill_id);
    if (!skill || skill->topic != question.topic || question.level < 1 || question.level > 10)
      throw std::invalid_argument("Curriculum question has no valid skill/level owner");
  }
  for (int level = 1; level <= 10; ++level) {
    std::set<std::string> families;
    bool application = false;
    for (const auto& id : bank.QuestionIds()) {
      const auto& question = bank.Get(id);
      if (question.topic != "containers" || question.level != level)
        continue;
      families.insert(question.question_family_id);
      application = application || question.evidence_type == QuestionEvidenceType::kApplication;
    }
    if (families.size() < 3 || !application)
      throw std::invalid_argument("Containers pilot needs three distinct families including application at L" +
                                  std::to_string(level));
  }
}

std::vector<std::string> ResolveCppLearningUnitQuestions(const QuestionBank& bank, std::string_view unit_id) {
  for (const auto& skill : SkillCatalog()) {
    for (int level = 1; level <= 10; ++level) {
      if (UnitId(skill.id, level) != unit_id)
        continue;
      const auto ids = bank.Select({.topics = {std::string(skill.topic)},
                                    .skill_ids = {std::string(skill.id)},
                                    .kinds = {},
                                    .min_level = level,
                                    .max_level = level,
                                    .preferred_question_ids = {}});
      if (ids.empty())
        throw std::invalid_argument("Неизвестный учебный раздел.");
      return ids;
    }
  }
  throw std::invalid_argument("Неизвестный учебный раздел.");
}

std::vector<std::string> CppLearningUnitQuestions(const QuestionBank& bank, const UserProgressSummary& progress,
                                                  std::string_view unit_id,
                                                  const std::unordered_map<std::string, QuestionReviewState>& reviews) {
  auto ids = ResolveCppLearningUnitQuestions(bank, unit_id);
  std::erase_if(ids, [&](const auto& id) { return !EligibleQuestion(bank.Get(id), progress, reviews); });
  if (ids.empty())
    throw std::invalid_argument("Этот раздел пока недоступен: проверь рабочую сложность и предварительные навыки.");
  return ids;
}

boost::json::object BuildCppCurriculum(const QuestionBank& bank, const UserProgressSummary& progress,
                                       const std::unordered_map<std::string, QuestionReviewState>& reviews,
                                       std::string_view topic) {
  if (!IsKnownTopic(topic))
    throw std::invalid_argument("Неизвестная тема C++.");
  const bool pilot = topic == Definition().at("pilot_topic").as_string();
  const int working = WorkingLevel(progress, topic);
  const auto topic_progress = std::ranges::find(progress.topics, topic, &TopicProgressSummary::topic);
  const AdaptivePromotionEvidence empty_promotion;
  const auto& promotion = topic_progress == progress.topics.end() ? empty_promotion : topic_progress->promotion;
  boost::json::array topics;
  for (const auto& item : TrainingTopics())
    topics.emplace_back(boost::json::object{{"id", item.id}, {"title", item.title}});
  boost::json::array levels;
  for (int level = 1; level <= 10; ++level) {
    std::map<std::string, std::vector<std::string>> grouped;
    for (const auto& id : bank.Select({.topics = {std::string(topic)},
                                       .skill_ids = {},
                                       .kinds = {},
                                       .min_level = level,
                                       .max_level = level,
                                       .preferred_question_ids = {}}))
      grouped[bank.Get(id).skill_id].push_back(id);
    std::set<std::string> level_families, application_families;
    std::int64_t completed = 0, due = 0, count = 0;
    boost::json::array units;
    for (const auto& [skill_id, ids] : grouped) {
      const auto& skill = *FindSkill(skill_id);
      std::set<std::string> theories, references, families, sources;
      boost::json::array questions, prerequisites;
      for (const auto& prerequisite : skill.prerequisites)
        prerequisites.emplace_back(
            boost::json::object{{"id", prerequisite}, {"title", FindSkill(prerequisite)->title}});
      for (const auto& id : ids) {
        const auto& question = bank.Get(id);
        const auto review = reviews.find(id);
        const bool answered = review != reviews.end() && review->second.correct_count > 0;
        const bool needs_review = review != reviews.end() && review->second.due;
        completed += answered ? 1 : 0;
        due += needs_review ? 1 : 0;
        ++count;
        families.insert(question.question_family_id);
        level_families.insert(question.question_family_id);
        if (question.evidence_type == QuestionEvidenceType::kApplication)
          application_families.insert(question.question_family_id);
        if (const auto* theory = FindTheoryForQuestion(question.skill_id, question.question_family_id))
          theories.emplace(theory->id);
        for (const auto* reference : ReferencesForQuestion(question))
          references.emplace(reference->id);
        if (question.source)
          sources.insert(*question.source);
        questions.emplace_back(boost::json::object{{"id", id},
                                                   {"public_id", question.public_id},
                                                   {"family_id", question.question_family_id},
                                                   {"completed", answered},
                                                   {"due", needs_review}});
      }
      const auto mastery = std::ranges::find(progress.skills, skill_id, &SkillProgressSummary::skill_id);
      units.emplace_back(boost::json::object{
          {"id", UnitId(skill_id, level)},
          {"skill_id", skill_id},
          {"title", skill.title},
          {"prerequisites", std::move(prerequisites)},
          {"questions", std::move(questions)},
          {"family_ids", Strings(families)},
          {"theory_ids", Strings(theories)},
          {"reference_ids", Strings(references)},
          {"sources", Strings(sources)},
          {"can_practice", SafeUnit(bank, progress, ids, reviews)},
          {"mastery", SkillMasteryStatusName(mastery == progress.skills.end() ? SkillMasteryStatus::kUnassessed
                                                                              : mastery->mastery_status)}});
    }
    const auto& goal = Definition().at("levels").as_array()[static_cast<std::size_t>(level - 1)].as_object();
    levels.emplace_back(boost::json::object{
        {"level", level},
        {"title", pilot ? goal.at("title") : boost::json::value("Каталог упражнений")},
        {"goal", pilot ? goal.at("goal") : boost::json::value("Учебная последовательность этой темы ещё готовится.")},
        {"unlocked", level <= progress.unlocked_level},
        {"count", count},
        {"family_count", static_cast<std::int64_t>(level_families.size())},
        {"completed", completed},
        {"due", due},
        {"content_ready", level_families.size() >= 3 && !application_families.empty()},
        {"units", std::move(units)}});
  }
  return {{"direction", "cpp"},
          {"topic", topic},
          {"title", *TopicTitle(topic)},
          {"pilot", pilot},
          {"working_level", working},
          {"unlocked_level", progress.unlocked_level},
          {"promotion_families", promotion.level == working ? static_cast<std::int64_t>(promotion.families.size()) : 0},
          {"passing_rule", Definition().at("passing_rule")},
          {"topics", std::move(topics)},
          {"levels", std::move(levels)}};
}
