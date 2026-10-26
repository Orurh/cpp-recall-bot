#include "learning/learning_route_catalog.h"

#include "learning/skill_catalog.h"

#include <algorithm>
#include <stdexcept>
#include <unordered_set>

namespace {
const std::vector<LearningRouteDefinition> kRoutes{
    {"containers_iterators",
     "Контейнеры и итераторы",
     {"containers"},
     {"vector::size", "vector::capacity", "vector::reserve", "vector::resize", "begin/end", "std::find",
      "vector::erase"},
     "containers.iterator_invalidation",
     {{"storage",
       "Размер и ёмкость",
       LearningRouteStageKind::kPractice,
       {"vector_reserve_does_not_change_size", "vector_resize_adds_elements"}},
      {"search", "Поиск и конец диапазона", LearningRouteStageKind::kPractice, {"find_missing_end_guard"}},
      {"erase", "Итератор после удаления", LearningRouteStageKind::kPractice, {"vector_erase_return_next"}},
      {"apply", "Самостоятельное применение", LearningRouteStageKind::kIndependent, {"vector_erase_filter_loop"}}}},
    {"map_lookup",
     "Словарь: поиск, вставка и обновление",
     {"containers"},
     {"map::find", "map::contains", "map::at", "map::operator[]", "map::try_emplace", "map::insert_or_assign"},
     "containers.map_lookup",
     {{"lookup", "Поиск без изменения словаря", LearningRouteStageKind::kPractice, {"map_find_without_insertion"}},
      {"update",
       "Вставка или обновление значения",
       LearningRouteStageKind::kPractice,
       {"map_subscript_inserts_default_value", "map_insert_or_assign_vs_try_emplace"}},
      {"apply", "Самостоятельное применение", LearningRouteStageKind::kIndependent, {"map_update_existing_stock"}}}},
    {"lifetime_raii",
     "Время жизни и RAII",
     {"language_core", "lifetime"},
     {"время жизни", "указатели и ссылки", "деструктор", "std::make_unique", "unique_ptr::get", "std::move",
      "unique_ptr::reset", "выход по исключению"},
     "lifetime.smart_pointers",
     {{"scope",
       "Когда заканчивается жизнь объекта",
       LearningRouteStageKind::kPractice,
       {"block_scope_destruction_order"}},
      {"borrow",
       "Доступ через указатель и ссылку",
       LearningRouteStageKind::kPractice,
       {"pointer_reference_share_object", "pointer_after_local_lifetime"}},
      {"cleanup", "Освобождение ресурса при return", LearningRouteStageKind::kPractice, {"vector_raii_early_return"}},
      {"create",
       "Создание владельца и get()",
       LearningRouteStageKind::kPractice,
       {"unique_ptr_make_get_observe", "unique_ptr_scope_deletes_object"}},
      {"move", "Передача владения", LearningRouteStageKind::kPractice, {"unique_ptr_move_transfers_ownership"}},
      {"reset", "Освобождение через reset()", LearningRouteStageKind::kPractice, {"unique_ptr_reset_destroys_now"}},
      {"unwind",
       "Освобождение на пути исключения",
       LearningRouteStageKind::kPractice,
       {"raii_unique_ptr_caught_exception"}},
      {"apply",
       "Самостоятельно: владелец и наблюдатель",
       LearningRouteStageKind::kIndependent,
       {"unique_ptr_borrow_across_owner_move"}}}},
};
}  // namespace

std::span<const LearningRouteDefinition> LearningRouteCatalog() {
  return kRoutes;
}

const LearningRouteDefinition* FindLearningRoute(std::string_view id) {
  const auto it = std::ranges::find(kRoutes, id, &LearningRouteDefinition::id);
  return it == kRoutes.end() ? nullptr : &*it;
}

void ValidateLearningRouteCatalog(const QuestionBank& bank) {
  std::unordered_set<std::string_view> routes;
  for (const auto& route : kRoutes) {
    if (route.id.empty() || route.title.empty() || !routes.insert(route.id).second || route.topics.empty() ||
        route.methods.empty() || route.stages.empty() || !IsMasteryPilotSkill(route.retention_skill_id)) {
      throw std::invalid_argument("invalid learning route definition");
    }
    std::unordered_set<std::string_view> stages, questions, families;
    bool independent = false;
    for (const auto& stage : route.stages) {
      if (stage.id.empty() || stage.title.empty() || stage.question_ids.empty() || stage.question_ids.size() > 2 ||
          !stages.insert(stage.id).second || (independent && stage.kind == LearningRouteStageKind::kPractice)) {
        throw std::invalid_argument("invalid learning route stage");
      }
      independent = independent || stage.kind == LearningRouteStageKind::kIndependent;
      for (const auto id : stage.question_ids) {
        const auto& question = bank.Get(id);
        if (!questions.insert(id).second || !families.insert(question.question_family_id).second ||
            std::ranges::find(route.topics, question.topic) == route.topics.end() ||
            (stage.kind == LearningRouteStageKind::kIndependent &&
             (question.evidence_type != QuestionEvidenceType::kApplication || !question.transfer ||
              question.skill_id != route.retention_skill_id))) {
          throw std::invalid_argument("invalid question in learning route");
        }
      }
    }
    if (!independent)
      throw std::invalid_argument("learning route has no independent application");
  }
}
