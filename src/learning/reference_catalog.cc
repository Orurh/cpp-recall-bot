#include "reference_catalog.h"

#include "quiz/question.h"

#include <algorithm>

namespace {

// Examples deliberately use other values than the cards. Help teaches the API,
// not the current answer. Tags are exact, editorial links, not substring guesses.
const std::vector<ReferenceEntry> kEntries{
    {"object_lifetime",
     "Время жизни: адрес не является владельцем",
     "Сырой указатель хранит адрес, но сам по себе не управляет временем жизни объекта. "
     "Нужно отдельно проверить существование объекта в момент каждого чтения или записи.",
     "При выходе из блока заканчивается жизнь его автоматических объектов. Временный объект "
     "обычно уничтожается в конце полного выражения; отдельные привязки ссылок имеют специальные "
     "правила продления жизни. Копирование адреса и получение указателя из метода сами её не продлевают.",
     "int number = 18;\nconst int* view = &number;\nnumber = 25;\n"
     "// Пока number жив, *view читает тот же объект и даст 25.\n"
     "// Копии указателя не являются отдельными копиями number.",
     "Считать, что сохранившийся адрес или прежнее содержимое памяти разрешают читать "
     "уже не существующий объект. Неопределённое поведение не означает обязательный сбой.",
     {"https://timsong-cpp.github.io/cppwp/n4950/basic.life",
      "https://timsong-cpp.github.io/cppwp/n4950/basic.stc.general",
      "https://timsong-cpp.github.io/cppwp/n4950/class.temporary"},
     {"dangling_pointer", "dangling_reference", "lifetime"}},
    {"string_c_str",
     "std::string::c_str() — доступ к буферу строки",
     "Заголовок <string>. c_str() возвращает указатель на символы конкретного std::string "
     "с завершающим нулевым символом. Отдельная владеющая копия строки не создаётся.",
     "Владелец буфера должен существовать во время использования указателя. "
     "Операции изменения строки могут инвалидировать указатель — проверяй гарантию конкретной "
     "операции. Через результат c_str() изменять символы нельзя. Константный доступ "
     "к символам не продлевает жизнь владельца.",
     "std::string label = \"ready\";\nconst char* first = label.c_str();\n"
     "// first[0] читается как 'r', пока label жив и указатель не инвалидирован.\n"
     "// Другая строка с тем же текстом не становится владельцем first.",
     "Принимать указатель за самостоятельную строку или считать, что он остаётся "
     "действительным после разрушения владельца и любых изменений его буфера.",
     {"https://timsong-cpp.github.io/cppwp/n4950/string.accessors",
      "https://timsong-cpp.github.io/cppwp/n4950/string.require",
      "https://timsong-cpp.github.io/cppwp/n4950/class.temporary"},
     {"c_str"}},
    {"unique_ownership",
     "std::unique_ptr: владение и временный доступ",
     "Заголовок <memory>. unique_ptr выражает единоличное владение ресурсом. "
     "Перемещающий конструктор передаёт хранимый указатель новому владельцу, "
     "а исходный unique_ptr становится пустым.",
     "get() возвращает хранимый указатель без передачи владения. reset() заменяет "
     "его и вызывает удалитель для прежнего непустого указателя; с обычным удалителем "
     "это уничтожает прежний объект. release() отдаёт указатель без вызова удалителя. "
     "Для чтения без передачи владения можно принять const T& или const T*: выбор "
     "зависит от того, допускается ли отсутствие объекта. Передача unique_ptr по значению выражает владение.",
     "auto owner = std::make_unique<int>(18);\nconst int& view = *owner;\n"
     "// view позволяет читать объект, но не становится его владельцем.\n"
     "// Функция void inspect(const int&) вызывается как inspect(*owner).",
     "Считать get() новым владельцем, использовать release() как удаление объекта "
     "или передавать unique_ptr по значению только ради чтения без намерения отдать ресурс.",
     {"https://timsong-cpp.github.io/cppwp/n4950/unique.ptr.single.ctor",
      "https://timsong-cpp.github.io/cppwp/n4950/unique.ptr.single.observers",
      "https://timsong-cpp.github.io/cppwp/n4950/unique.ptr.single.modifiers",
      "https://timsong-cpp.github.io/cppwp/n4950/unique.ptr.dltr.dflt"},
     {"unique_ptr", "borrowing"}},
    {"scope_cleanup",
     "RAII: освобождение при выходе из области",
     "RAII связывает ресурс с владеющим объектом и его деструктором. "
     "При обычном выходе из блока или функции, включая return, уничтожаются "
     "созданные автоматические объекты покидаемых областей.",
     "При переходе от throw к подходящему обработчику идёт раскрутка стека: "
     "уничтожаются построенные объекты покидаемых областей, в обратном порядке построения. "
     "Объект снаружи покидаемого блока может оставаться живым. Например, деструктор "
     "lock_guard освобождает блокировку, но не подавляет исключение. Это не обещание "
     "очистки при любом аварийном завершении процесса.",
     "{\n  std::vector<int> buffer(24);\n  use(buffer);\n}\n"
     "// При обычном выходе из блока деструктор buffer освободит его хранилище.\n"
     "// Для чужого невладеющего указателя такое освобождение не выполняется.",
     "Вручную удалять внутреннее хранилище контейнера, забывать об уже построенных "
     "объектах на пути исключения или считать, что после catch уничтожены также все внешние объекты.",
     {"https://timsong-cpp.github.io/cppwp/n4950/stmt.dcl", "https://timsong-cpp.github.io/cppwp/n4950/stmt.jump",
      "https://timsong-cpp.github.io/cppwp/n4950/except.ctor",
      "https://timsong-cpp.github.io/cppwp/n4950/thread.lock.guard",
      "https://timsong-cpp.github.io/cppwp/n4950/vector.overview"},
     {"raii", "early_return", "stack_unwinding", "lock_guard"}},
    {"parameter_packs",
     "Пакеты параметров: несколько аргументов вместо одного",
     "template <typename... Ts> объявляет пакет типов Ts. В записи Ts... values объявляется "
     "пакет параметров функции: отдельный параметр для каждого переданного аргумента.",
     "Пакет может быть пустым. sizeof...(Ts) и sizeof...(values) считают элементы соответствующего "
     "пакета, а не байты. В вызове target(values...) пакет раскрывается в список отдельных "
     "аргументов: это не один контейнер и не свёртка оператором. Образец может быть выражением: "
     "target((values * 2)...) удваивает каждый аргумент перед одним вызовом target.",
     "template <typename... Ts>\nvoid relay(Ts... values) {\n  target(values...);\n}\n"
     "// relay(8, 5) вызывает target(8, 5).\n// Целевой вызов должен быть допустим для этих аргументов.",
     "Пытаться индексировать пакет как массив или путать sizeof...(Ts) с sizeof одного типа. "
     "Пакет типов и пакет параметров связаны, но это не одна переменная.",
     {"https://timsong-cpp.github.io/cppwp/n4950/temp.variadic",
      "https://timsong-cpp.github.io/cppwp/n4950/expr.sizeof"},
     {"parameter_pack_basics", "pack_expansion", "fold_expression"}},
    {"fold_expressions",
     "Свёртка: объединение элементов оператором",
     "Свёртка (fold expression), начиная с C++17, объединяет элементы пакета бинарным "
     "оператором. Например, (... + values) складывает значения непустого пакета.",
     "(... op values) — левая свёртка: ((v1 op v2) op v3). "
     "(values op ...) — правая: (v1 op (v2 op v3)). Это группировка выражения, "
     "а не общее обещание порядка вычисления аргументов. Для встроенного && результат "
     "истинен, только если истинны все операнды; bool без boolalpha выводится как 1 или 0.",
     "template <typename... Ts>\nauto total(Ts... values) {\n  return (... + values);\n}\n"
     "// total(3, 8, 5): (3 + 8) + 5.\n// Для вычитания направление меняет результат.",
     "Считать, что многоточие означает цикл во время выполнения, или переносить "
     "результат непустой свёртки на пустой пакет без отдельной проверки правила.",
     {"https://timsong-cpp.github.io/cppwp/n4950/expr.prim.fold",
      "https://timsong-cpp.github.io/cppwp/n4950/temp.variadic"},
     {"fold_expression"}},
    {"empty_fold",
     "Пустой пакет: отдельное правило свёртки",
     "Для унарной свёртки пустого пакета стандарт задаёт результаты только для "
     "&& (true), || (false) и запятой (void()). Унарная свёртка пустого пакета с + некорректна.",
     "В бинарной свёртке (init op ... op values) начальное выражение записано явно. "
     "Если пакет пуст, результат — init. Например, единицу произведения выбирает "
     "автор функции, а не компилятор автоматически по оператору *.",
     "template <typename... Ts>\nauto product(Ts... values) {\n  return (1 * ... * values);\n}\n"
     "// product() даёт 1, product(4, 5) — (1 * 4) * 5.",
     "Ожидать, что компилятор сам подставит 0 для любой пустой суммы, или считать "
     "корректность пустого пакета общим свойством всех унарных свёрток.",
     {"https://timsong-cpp.github.io/cppwp/n4950/temp.variadic"},
     {"empty_pack"}},
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
