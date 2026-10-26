#include "skill_catalog.h"

#include "quiz/topic_catalog.h"

#include <functional>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <unordered_set>

namespace {

const std::vector<SkillDefinition> kSkills{
    {"core.scope", "language_core", "Области видимости и поиск имён", {}},
    {"core.undefined_behavior", "language_core", "Неопределённое поведение", {}},
    {"core.const", "language_core", "Константность", {}},
    {"core.value_categories", "language_core", "Категории значений", {}},
    {"core.storage_duration", "language_core", "Хранение и размещение объектов", {}},
    {"build.translation_units", "language_core", "Единицы трансляции и заголовки", {"core.scope"}},
    {"build.compilation_linking", "language_core", "Компиляция и компоновка", {"build.translation_units"}},
    {"build.linkage", "language_core", "Связывание имён", {"build.translation_units"}},
    {"build.odr", "language_core", "Правило одного определения", {"build.linkage"}},
    {"build.abi",
     "language_core",
     "Бинарный интерфейс приложения (ABI)",
     {"build.compilation_linking", "build.linkage"}},
    {"functions.default_arguments", "language_core", "Аргументы по умолчанию", {"core.scope"}},
    {"core.copy_semantics", "language_core", "Семантика копирования", {"core.storage_duration"}},
    {"raii.fundamentals", "language_core", "Основы RAII", {"core.storage_duration"}},
    {"references.fundamentals", "references", "Основы ссылок", {"core.value_categories"}},
    {"lifetime.objects", "lifetime", "Время жизни объектов", {"core.storage_duration"}},
    {"lifetime.smart_pointers", "lifetime", "Умные указатели", {"lifetime.objects", "raii.fundamentals"}},
    {"special_members.copy_construction",
     "special_member_functions",
     "Конструкторы копирования",
     {"core.copy_semantics", "references.fundamentals", "lifetime.objects"}},
    {"move.fundamentals",
     "move_semantics",
     "Основы семантики перемещения",
     {"core.value_categories", "lifetime.objects"}},
    {"oop.objects", "oop", "Объекты и классы", {"core.scope"}},
    {"oop.encapsulation", "oop", "Инкапсуляция", {"oop.objects"}},
    {"oop.abstraction", "oop", "Абстракция и интерфейсы", {"oop.encapsulation"}},
    {"oop.inheritance", "oop", "Наследование", {"oop.objects"}},
    {"oop.polymorphism", "oop", "Полиморфизм", {"oop.objects"}},
    {"oop.static_polymorphism", "oop", "Статический полиморфизм", {"oop.polymorphism"}},
    {"oop.dynamic_polymorphism", "oop", "Динамический полиморфизм", {"oop.polymorphism", "oop.inheritance"}},
    {"oop.virtual_functions", "oop", "Виртуальные функции", {"oop.dynamic_polymorphism"}},
    {"oop.abstract_classes", "oop", "Абстрактные классы", {"oop.virtual_functions"}},
    {"oop.virtual_destruction", "oop", "Виртуальное разрушение", {"oop.virtual_functions", "lifetime.objects"}},
    {"oop.object_slicing", "oop", "Срезка объектов", {"oop.inheritance", "core.copy_semantics"}},
    {"oop.composition", "oop", "Композиция и наследование", {"oop.inheritance"}},
    {"oop.overload_override",
     "oop",
     "Перегрузка и переопределение",
     {"oop.static_polymorphism", "oop.virtual_functions"}},
    {"containers.vector", "containers", "std::vector", {"core.storage_duration"}},
    {"containers.iterator_invalidation", "containers", "Инвалидация итераторов", {"containers.vector"}},
    {"containers.map_lookup", "containers", "Поиск в std::map", {}},
    {"containers.unordered", "containers", "Хеш-контейнеры", {}},
    {"algorithms.stl", "containers", "Алгоритмы стандартной библиотеки", {"containers.vector"}},
    {"ranges.fundamentals", "containers", "Диапазоны (ranges) и заимствованные диапазоны", {"algorithms.stl"}},
    {"library.non_owning_views", "containers", "Невладеющие представления", {"core.storage_duration"}},
    {"library.callables", "containers", "Вызываемые объекты и std::invoke", {"oop.objects", "references.fundamentals"}},
    {"library.vocabulary_types", "containers", "Типы-словари стандартной библиотеки", {"core.copy_semantics"}},
    {"algorithms.sorting", "language_core", "Алгоритмы сортировки", {}},
    {"templates.instantiation", "templates", "Инстанцирование шаблонов", {"core.scope"}},
    {"templates.deduction",
     "templates",
     "Вывод аргументов шаблона",
     {"core.value_categories", "references.fundamentals"}},
    {"templates.constexpr", "templates", "if constexpr и зависимые выражения", {"templates.instantiation"}},
    {"templates.type_traits", "templates", "Преобразования и вычисления типов", {"templates.deduction"}},
    {"templates.concepts",
     "templates",
     "Ограничения шаблонов (concepts) и requires-выражения",
     {"templates.deduction", "templates.instantiation"}},
    {"templates.parameter_packs", "templates", "Пакеты параметров и их раскрытие", {"templates.deduction"}},
    {"templates.fold_expressions", "templates", "Свёртки непустого пакета", {"templates.parameter_packs"}},
    {"templates.variadic", "templates", "Свёртки: пустой пакет и начальное значение", {"templates.fold_expressions"}},
    {"concurrency.execution", "concurrency", "Потоки выполнения", {}},
    {"concurrency.primitives", "concurrency", "Примитивы синхронизации", {"concurrency.execution"}},
    {"concurrency.memory_model",
     "concurrency",
     "Модель памяти и гонки данных",
     {"concurrency.execution", "core.undefined_behavior"}},
    {"concurrency.atomics",
     "concurrency",
     "Атомарные операции и порядок операций с памятью (memory ordering)",
     {"concurrency.memory_model"}},
    {"concurrency.async", "concurrency", "Асинхронные задачи и их результаты (future)", {"concurrency.execution"}},
};

const std::vector<std::string_view> kMasteryPilotSkills{
    "core.storage_duration",
    "raii.fundamentals",
    "lifetime.objects",
    "lifetime.smart_pointers",
    "containers.iterator_invalidation",
    "containers.map_lookup",
    "containers.vector",
    "concurrency.atomics",
    "concurrency.async",
    "oop.dynamic_polymorphism",
    "oop.virtual_functions",
    "oop.encapsulation",
    "oop.inheritance",
    "oop.objects",
    "oop.static_polymorphism",
    "oop.object_slicing",
    "oop.overload_override",
    "templates.instantiation",
    "templates.deduction",
    "special_members.copy_construction",
};

}  // namespace

std::span<const SkillDefinition> SkillCatalog() {
  return kSkills;
}

std::span<const std::string_view> MasteryPilotSkillIds() {
  return kMasteryPilotSkills;
}

bool IsMasteryPilotSkill(std::string_view id) {
  return std::ranges::find(kMasteryPilotSkills, id) != kMasteryPilotSkills.end();
}

const SkillDefinition* FindSkill(std::string_view id) {
  for (const auto& skill : kSkills) {
    if (skill.id == id) {
      return &skill;
    }
  }
  return nullptr;
}

void ValidateSkillCatalog() {
  std::unordered_map<std::string_view, const SkillDefinition*> by_id;
  by_id.reserve(kSkills.size());

  for (const auto& skill : kSkills) {
    if (skill.id.empty() || skill.title.empty()) {
      throw std::logic_error("skill id and title must not be empty");
    }
    if (!IsKnownTopic(skill.topic)) {
      throw std::logic_error("skill '" + std::string(skill.id) + "' has unknown topic");
    }
    if (!by_id.emplace(skill.id, &skill).second) {
      throw std::logic_error("duplicate skill id: " + std::string(skill.id));
    }
  }

  for (const auto& skill : kSkills) {
    std::unordered_set<std::string_view> unique;
    for (const auto prerequisite : skill.prerequisites) {
      if (prerequisite == skill.id) {
        throw std::logic_error("skill cannot depend on itself: " + std::string(skill.id));
      }
      if (!by_id.contains(prerequisite)) {
        throw std::logic_error("skill '" + std::string(skill.id) +
                               "' has unknown prerequisite: " + std::string(prerequisite));
      }
      if (!unique.insert(prerequisite).second) {
        throw std::logic_error("skill '" + std::string(skill.id) + "' has duplicate prerequisite");
      }
    }
  }

  enum class VisitState { kVisiting, kVisited };
  std::unordered_map<std::string_view, VisitState> state;
  state.reserve(kSkills.size());

  const std::function<void(const SkillDefinition&)> visit = [&](const SkillDefinition& skill) {
    const auto current = state.find(skill.id);
    if (current != state.end()) {
      if (current->second == VisitState::kVisiting) {
        throw std::logic_error("skill prerequisite cycle contains: " + std::string(skill.id));
      }
      return;
    }

    state.emplace(skill.id, VisitState::kVisiting);
    for (const auto prerequisite : skill.prerequisites) {
      visit(*by_id.at(prerequisite));
    }
    state.insert_or_assign(skill.id, VisitState::kVisited);
  };

  for (const auto& skill : kSkills) {
    visit(skill);
  }

  if (kMasteryPilotSkills.size() < 15 || kMasteryPilotSkills.size() > 25) {
    throw std::logic_error("mastery pilot must contain between 15 and 25 skills");
  }

  std::unordered_set<std::string_view> pilot_skills;
  for (const auto skill_id : kMasteryPilotSkills) {
    if (!by_id.contains(skill_id)) {
      throw std::logic_error("mastery pilot references unknown skill: " + std::string(skill_id));
    }
    if (!pilot_skills.insert(skill_id).second) {
      throw std::logic_error("mastery pilot contains duplicate skill: " + std::string(skill_id));
    }
  }
}
