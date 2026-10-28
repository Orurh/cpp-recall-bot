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
    {"template_packs_folds",
     "Шаблоны: пакеты параметров и свёртки",
     {"templates"},
     {"typename... Ts", "Ts... values", "sizeof...", "раскрытие пакета", "свёртки", "пустой пакет",
      "начальное значение"},
     "templates.variadic",
     {{"packs",
       "Пакеты и раскрытие аргументов",
       LearningRouteStageKind::kPractice,
       {"parameter_pack_element_count", "parameter_pack_expand_call"}},
      {"apply_packs",
       "Самостоятельно: список аргументов",
       LearningRouteStageKind::kIndependent,
       {"parameter_pack_frame_arguments"}},
      {"folds",
       "Свёртки непустого пакета",
       LearningRouteStageKind::kPractice,
       {"fold_and_nonempty_pack", "fold_subtraction_grouping"}},
      {"apply_folds",
       "Самостоятельно: проверка измерений",
       LearningRouteStageKind::kIndependent,
       {"fold_validate_each_measurement"}},
      {"empty",
       "Пустой пакет и начальное значение",
       LearningRouteStageKind::kPractice,
       {"fold_and_empty_pack_identity", "binary_fold_empty_pack_initial_value"}},
      {"apply_empty",
       "Самостоятельно: базовая стоимость и доплаты",
       LearningRouteStageKind::kIndependent,
       {"binary_fold_registration_fee"}}}},
    {"values_references",
     "Ссылки и категории значений",
     {"language_core", "references"},
     {"lvalue и prvalue", "int и int&", "const int&", "параметры функции", "возврат ссылки"},
     "references.fundamentals",
     {{"expressions",
       "Объект и вычисленное значение",
       LearningRouteStageKind::kPractice,
       {"value_category_name_and_sum"}},
      {"alias",
       "Ссылка и отдельное значение",
       LearningRouteStageKind::kPractice,
       {"reference_definition", "reference_alias_and_copy"}},
      {"access",
       "Передача в функцию и const-доступ",
       LearningRouteStageKind::kPractice,
       {"reference_parameter_updates_argument", "const_reference_observes_changes"}},
      {"apply_expression",
       "Самостоятельно: результат вызова",
       LearningRouteStageKind::kIndependent,
       {"value_category_reference_call_assignment"}},
      {"apply_reference",
       "Самостоятельно: тип результата",
       LearningRouteStageKind::kIndependent,
       {"reference_return_existing_member"}}}},
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
    const auto* target = FindSkill(route.retention_skill_id);
    if (route.id.empty() || route.title.empty() || !routes.insert(route.id).second || route.topics.empty() ||
        route.methods.empty() || route.stages.empty() || target == nullptr ||
        std::ranges::find(route.topics, target->topic) == route.topics.end() ||
        route.stages.front().kind != LearningRouteStageKind::kPractice) {
      throw std::invalid_argument("invalid learning route definition");
    }
    std::unordered_set<std::string_view> stages, questions, families, introduced_skills;
    bool independent_target = false;
    for (const auto& stage : route.stages) {
      if (stage.id.empty() || stage.title.empty() || stage.question_ids.empty() || stage.question_ids.size() > 2 ||
          !stages.insert(stage.id).second) {
        throw std::invalid_argument("invalid learning route stage");
      }
      for (const auto id : stage.question_ids) {
        const auto& question = bank.Get(id);
        if (!questions.insert(id).second || !families.insert(question.question_family_id).second ||
            std::ranges::find(route.topics, question.topic) == route.topics.end() ||
            (stage.kind == LearningRouteStageKind::kIndependent &&
             (question.evidence_type != QuestionEvidenceType::kApplication || !question.transfer ||
              !introduced_skills.contains(question.skill_id)))) {
          throw std::invalid_argument("invalid question in learning route");
        }
        if (stage.kind == LearningRouteStageKind::kPractice)
          introduced_skills.insert(question.skill_id);
        independent_target = independent_target || (stage.kind == LearningRouteStageKind::kIndependent &&
                                                    question.skill_id == route.retention_skill_id);
      }
    }
    if (!independent_target)
      throw std::invalid_argument("learning route has no independent application of its retention target");
  }
}
