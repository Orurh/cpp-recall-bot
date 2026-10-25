#include "reference_catalog.h"

#include "quiz/question.h"

#include <algorithm>

namespace {

// Examples deliberately use other values than the cards. Help teaches the API,
// not the current answer. Tags are exact, editorial links, not substring guesses.
const std::vector<ReferenceEntry> kEntries{
    {"invoke",
     "std::invoke — единый способ вызова",
     "Заголовок <functional>. std::invoke(f, args...) вызывает функцию или функциональный объект; "
     "для указателя на член первым аргументом после f передаётся объект.",
     "std::invoke(&Type::field, object) обращается к полю. Для метода передай объект, затем аргументы "
     "метода. Возвращаемый тип и допустимость вызова зависят от константности и категории объекта.",
     "struct Box { int weight; };\nBox box{42};\nauto& weight = std::invoke(&Box::weight, box);\nweight = 50; // "
     "меняет box.weight",
     "Указатель на поле — не свободная функция. Для обычного объекта нужен доступ object.*member; "
     "std::invoke выбирает эту форму сам. auto без & может скопировать результат.",
     {"https://timsong-cpp.github.io/cppwp/n4950/func.invoke",
      "https://timsong-cpp.github.io/cppwp/n4950/func.require"},
     {"invoke", "invoke_result"}},
    {"invoke_result",
     "std::invoke_result_t — тип результата вызова",
     "Заголовок <type_traits>. std::invoke_result_t<F, Args...> — тип результата допустимого "
     "вызова по правилам std::invoke. Это вычисление типа при компиляции, а не выполнение функции.",
     "Указывай тип вызываемого объекта и точные типы аргументов, включая ссылки и const. "
     "Результат может быть ссылкой. Если вызов недопустим, у invoke_result нет вложенного type.",
     "struct Meter { double value; };\nusing R = std::invoke_result_t<decltype(&Meter::value), const Meter&>;\n// R — "
     "const double&",
     "Считать, что суффикс _t убирает ссылки, или ожидать вызова функции и её побочных эффектов.",
     {"https://timsong-cpp.github.io/cppwp/n4950/meta.trans.other",
      "https://timsong-cpp.github.io/cppwp/n4950/func.require"},
     {"invoke_result"}},
    {"remove_cvref",
     "std::remove_cvref_t — убрать ссылку и верхний const",
     "Заголовок <type_traits>, C++20. std::remove_cvref_t<T> сначала убирает ссылку, "
     "затем верхнеуровневые const и volatile у полученного типа.",
     "Это преобразование типа, не объекта. const у объекта, на который указывает указатель, "
     "не является верхнеуровневым const самого указателя и сохраняется.",
     "using A = std::remove_cvref_t<const double&>; // double\nusing B = std::remove_cvref_t<const double* const&>; // "
     "const double*",
     "Ожидать удаления всех const на любой глубине или снятия константности существующей переменной.",
     {"https://timsong-cpp.github.io/cppwp/n4950/meta.trans.other",
      "https://timsong-cpp.github.io/cppwp/n4950/meta.trans.cv"},
     {"remove_cvref"}},
    {"lower_bound",
     "std::lower_bound — граница, не точное совпадение",
     "Заголовок <algorithm>. lower_bound(first, last, key, comp) возвращает первый итератор, "
     "для которого comp(*it, key) ложно, либо last. Без comp для обычных чисел используется порядок по возрастанию.",
     "Для данного key сначала должны идти все элементы с истинным comp(element, key), затем остальные. "
     "Сортировка по согласованному порядку обеспечивает это. С std::greater<> граница ищется в порядке убывания. "
     "Число сравнений логарифмическое; с forward-итераторами перемещения могут быть линейными.",
     "std::vector<int> values{3, 6, 9};\nauto it = std::lower_bound(values.begin(), values.end(), 5);\n// it указывает "
     "на 6; ключа 5 в наборе нет",
     "Разыменовать end() или принять любой найденный кандидат за точное совпадение. "
     "Для порядка эквивалентность проверяется в обоих направлениях сравнения.",
     {"https://timsong-cpp.github.io/cppwp/n4950/lower.bound",
      "https://timsong-cpp.github.io/cppwp/n4950/alg.binary.search"},
     {"lower_bound"}},
    {"span",
     "std::span — представление без владения",
     "Заголовок <span>, C++20. std::span<T> представляет непрерывную последовательность чужих объектов. "
     "Он не копирует элементы и не продлевает их жизнь.",
     "std::span<const T> запрещает изменение элементов через представление. const std::span<T> "
     "делает константным сам объект представления, но не T. В C++23 operator[] требует индекс меньше size().",
     "int values[]{4, 8};\nstd::span<const int> view = values;\nvalues[1] = 12;\n// view[1] теперь 12; данные общие",
     "Возвращать span на локальный массив, использовать его после разрушения владельца или "
     "перевыделения буфера vector. Константный доступ не означает отдельную неизменяемую копию.",
     {"https://timsong-cpp.github.io/cppwp/n4950/views.span", "https://timsong-cpp.github.io/cppwp/n4950/span.elem"},
     {"span"}},
    {"ordering",
     "Правило порядка и эквивалентность ключей",
     "comp(a, b) означает: a идёт раньше b. Эквивалентность по порядку — "
     "!comp(a, b) && !comp(b, a), а не обязательно a == b.",
     "Для сортировки требуется строгий слабый порядок: comp(x, x) ложно, порядок и "
     "полученная эквивалентность транзитивны. std::less<> из <functional> для чисел "
     "задаёт порядок через <; типы аргументов выводятся при вызове.",
     "auto byLength = [](const auto& a, const auto& b) { return a.size() < b.size(); };\n// Строки разного содержания "
     "могут быть эквивалентны по длине.",
     "Использовать <= как строгий порядок или считать, что эквивалентные ключи обязаны "
     "иметь одинаковое содержимое.",
     {"https://timsong-cpp.github.io/cppwp/n4950/alg.sorting.general",
      "https://timsong-cpp.github.io/cppwp/n4950/comparisons"},
     {"strict_weak_ordering", "comparator_equivalence", "transparent_comparator", "sort_projection"}},
    {"sort_projection",
     "std::ranges::sort — сортировка с извлечением ключа",
     "Заголовок <algorithm>, C++20. ranges::sort(range, comp, projection) сортирует диапазон, "
     "сравнивая результаты projection для его элементов.",
     "Проекция отвечает за получение ключа, компаратор — за порядок ключей. В качестве "
     "проекции допустим указатель на поле. Нужны итераторы случайного доступа и переставляемые элементы; "
     "число сравнений и применений проекции — O(N log N).",
     "struct Row { int rank; };\nstd::vector<Row> rows{{6}, {2}};\nstd::ranges::sort(rows, {}, &Row::rank);\n// Записи "
     "упорядочены по rank; {} выбирает компаратор по умолчанию.",
     "Поменять местами компаратор и проекцию или ожидать, что алгоритм возвращает новую копию, "
     "не меняя переданный диапазон.",
     {"https://timsong-cpp.github.io/cppwp/n4950/sort",
      "https://timsong-cpp.github.io/cppwp/n4950/alg.sorting.general"},
     {"sort_projection"}},
};

}  // namespace

std::span<const ReferenceEntry> ReferenceCatalog() {
  return kEntries;
}

std::vector<const ReferenceEntry*> ReferencesForQuestion(const Question& question) {
  std::vector<const ReferenceEntry*> result;
  for (const auto& entry : kEntries) {
    if (std::ranges::any_of(entry.tags, [&](std::string_view tag) {
          return std::ranges::find(question.tags, tag) != question.tags.end();
        })) {
      result.push_back(&entry);
    }
  }
  return result;
}
