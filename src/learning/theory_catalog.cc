#include "theory_catalog.h"

#include "learning/module_catalog.h"
#include "learning/skill_catalog.h"

#include <algorithm>
#include <stdexcept>
#include <string>
#include <unordered_set>

namespace {

const std::vector<TheoryBlock> kTheoryBlocks{
    {
        .id = "value_categories.expression_identity",
        .skill_id = "core.value_categories",
        .title = "Что обозначает выражение",
        .principle = "Категория значения относится к выражению, а не к его позиции в присваивании. "
                     "Для обычного int имя объекта — lvalue, арифметическое вычисление — prvalue.",
        .mechanism = "lvalue обозначает объект или функцию. Вызов с результатом int& тоже lvalue; "
                     "вызов с результатом int — prvalue. Встроенное присваивание требует слева изменяемое "
                     "lvalue. Категории prvalue и xvalue вместе образуют rvalue; не нужно считать "
                     "все выражения справа от = временными объектами.",
        .example = "int main() {\n  int width = 2;\n  const int& view = width;\n  int next = view + 1;\n"
                   "  (void)next;\n}\n"
                   "// view — lvalue: обозначает width, хотя запрещает запись через view.\n"
                   "// view + 1 — prvalue int, вычисление отдельного значения.",
        .common_mistake = "Считать всякое lvalue изменяемым или выводить категорию результата из типа параметров.",
        .sources = {"https://timsong-cpp.github.io/cppwp/n4950/basic.lval",
                    "https://timsong-cpp.github.io/cppwp/n4950/expr.call#13",
                    "https://timsong-cpp.github.io/cppwp/n4950/expr.ass#1"},
    },
    {
        .id = "references.same_object_access",
        .skill_id = "references.fundamentals",
        .title = "Другой путь к существующему объекту",
        .principle = "После привязки ссылка обозначает тот же объект. Запись меняет его значение, "
                     "а не объект, с которым связана ссылка.",
        .mechanism = "Параметр int& связан с аргументом, а обычный int получает отдельное значение. "
                     "Ссылочный результат может обозначать поле объекта вызывающего кода, но этот объект "
                     "должен оставаться живым при обращении через результат. const int& ограничивает путь "
                     "записи, а не создаёт снимок данных.",
        .example = "void Exchange(int& left, int& right) {\n  int saved = left;\n  left = right;\n"
                   "  right = saved;\n}\n"
                   "// saved хранит отдельное значение. left и right не перепривязываются: меняются их объекты.",
        .common_mistake = "Считать ссылку владельцем, перепривязывать её присваиванием или возвращать доступ "
                          "к уже уничтоженному локальному объекту.",
        .sources = {"https://timsong-cpp.github.io/cppwp/n4950/dcl.init.ref",
                    "https://timsong-cpp.github.io/cppwp/n4950/dcl.type.cv#3",
                    "https://timsong-cpp.github.io/cppwp/n4950/expr.call#6",
                    "https://timsong-cpp.github.io/cppwp/n4950/basic.life"},
    },
    {
        .id = "srp.change_boundary",
        .skill_id = "oop.srp",
        .title = "Причина изменения и актор",
        .principle = "SRP отделяет независимые источники требований, а не ограничивает класс одним методом.",
        .mechanism = "Актор — роль или согласованная группа, представляющая одну бизнес-функцию. "
                     "Спрашивай, кто потребует изменить правило и какие чужие правила могут пострадать. "
                     "Одного названия класса или числа пользователей для вывода недостаточно.",
        .example = "Модуль подсчёта результатов спортивного турнира обслуживает правила судейской коллегии.\n"
                   "Модуль рекламных показов обслуживает договорённости со спонсорами.\n"
                   "Общие данные турнира не делают эти две причины изменения одной ответственностью.",
        .common_mistake =
            "Дробить связанную бизнес-функцию на классы по числу методов или путать актора с автором кода.",
        .sources = {"https://blog.cleancoder.com/uncle-bob/2014/05/08/SingleReponsibilityPrinciple.html"},
        .source_kind = LearningSourceKind::kDesignPrinciple,
    },
    {
        .id = "sfinae.substitution_boundary",
        .skill_id = "templates.sfinae",
        .title = "Неудача подстановки — не любая ошибка шаблона",
        .principle = "SFINAE может исключить кандидат при подстановке в непосредственный контекст. "
                     "Оно не означает, что любая ошибка шаблонного кода игнорируется.",
        .mechanism = "Сначала формируются кандидаты и выбирается перегрузка. Затем при необходимости "
                     "инстанцируется её тело. Ошибка на втором шаге не запускает выбор перегрузки заново. "
                     "requires и SFINAE позволяют проверять пригодность раньше, но механизмы различаются.",
        .example = "template <typename T>\nauto coordinate(T v) -> decltype(v.x);\n"
                   "int coordinate(...);\n"
                   "// Для T без x первая декларация может отпасть при подстановке.\n"
                   "// Это не гарантия подавления ошибок в теле уже выбранной функции.",
        .common_mistake =
            "Переносить зависимую проверку в тело и рассчитывать, что запасная перегрузка спасёт компиляцию.",
        .sources = {"https://timsong-cpp.github.io/cppwp/n4950/temp.deduct"},
    },
    {
        .id = "parameter_packs.basics",
        .skill_id = "templates.parameter_packs",
        .title = "Пакеты типов и параметров",
        .principle = "template <typename... Ts> объявляет пакет типов. Ts... values задаёт отдельные параметры "
                     "функции для переданных аргументов. Это не массив и не один общий тип.",
        .mechanism = "sizeof...(Ts) считает типы, sizeof...(values) — параметры, не байты. "
                     "В записи target(values...) пакет раскрывается в список отдельных аргументов одного вызова. "
                     "Число и типы аргументов должны подходить целевой функции.",
        .example = "template <typename... Ts>\nvoid relay(Ts... values) {\n  target(values...);\n}\n"
                   "// relay(8, 5) образует вызов target(8, 5), если он допустим.",
        .common_mistake = "Пытаться обращаться к values[i] как к элементу контейнера или ожидать, что "
                          "target(values...) вызывает target отдельно для каждого аргумента.",
        .sources = {"https://timsong-cpp.github.io/cppwp/n4950/temp.variadic",
                    "https://timsong-cpp.github.io/cppwp/n4950/expr.sizeof"},
    },
    {
        .id = "fold_expressions.nonempty",
        .skill_id = "templates.fold_expressions",
        .title = "Свёртка непустого пакета",
        .principle = "Свёртка объединяет элементы пакета бинарным оператором. Для непустого пакета "
                     "(... + values) строит цепочку сложений, а (... && values) — цепочку логических И.",
        .mechanism = "Левая форма (... op values) даёт (v1 op v2) op v3; правая (values op ...) — "
                     "v1 op (v2 op v3). Направление определяет группировку, но не является общим правилом "
                     "порядка вычисления операндов. Для встроенного && ложного значения достаточно для false.",
        .example = "// Для значений 15, 4, 3:\n// (... - values) -> (15 - 4) - 3\n"
                   "// (values - ...) -> 15 - (4 - 3)",
        .common_mistake = "Переносить одинаковый результат сложения на вычитание или ожидать, "
                          "что правило непустого пакета само определит результат пустого.",
        .sources = {"https://timsong-cpp.github.io/cppwp/n4950/expr.prim.fold",
                    "https://timsong-cpp.github.io/cppwp/n4950/temp.variadic"},
    },
    {
        .id = "fold_expressions.empty",
        .skill_id = "templates.variadic",
        .title = "Свёртка пустого пакета",
        .principle = "У пустого пакета нет операндов для обычной цепочки. Для унарной свёртки стандарт "
                     "отдельно определяет && как true, || как false и запятую как void().",
        .mechanism = "Другие операторы не допускают пустую унарную свёртку. "
                     "Бинарная свёртка содержит явное начальное выражение: при пустом пакете оно и остаётся. "
                     "Нейтральное значение не подбирается автоматически по оператору.",
        .example = "template <typename... Ts>\nauto product(Ts... values) {\n"
                   "  return (1 * ... * values);\n}\n// product() даёт 1; product(4, 5) даёт 20.",
        .common_mistake = "Считать пустую унарную сумму корректной с результатом 0 или "
                          "путать явно записанное начальное выражение с элементом пакета.",
        .sources = {"https://timsong-cpp.github.io/cppwp/n4950/temp.variadic"},
    },
    {
        .id = "storage_duration.basics",
        .skill_id = "core.storage_duration",
        .title = "Длительность хранения и существование объекта",
        .principle =
            "Длительность хранения определяет, как долго доступна память для объекта. Она не заменяет понятие "
            "времени жизни: в одной области памяти объекты могут последовательно начинать и завершать существование.",
        .mechanism =
            "Автоматическая длительность хранения обычно связана с входом в блок и выходом из него, статическая — "
            "с выполнением программы, потоковая — с выполнением потока, динамическая управляется выделением и "
            "освобождением памяти. Область видимости имени, доступность памяти и время жизни объекта — разные вещи.",
        .example = "int calls = 0; // статическая длительность хранения\n\n"
                   "void record() {\n  int step = 1; // автоматическая длительность хранения\n  calls += step;\n}\n"
                   "// calls сохраняется между вызовами record().\n"
                   "// Для каждого вызова создаётся свой step, который уничтожается при возврате.",
        .common_mistake =
            "Считать, что сохранённый адрес продлевает существование автоматического объекта или делает обращение к "
            "нему допустимым после выхода из блока.",
        .sources = {"https://timsong-cpp.github.io/cppwp/n4950/basic.stc.general",
                    "https://timsong-cpp.github.io/cppwp/n4950/basic.stc.static",
                    "https://timsong-cpp.github.io/cppwp/n4950/basic.stc.auto",
                    "https://timsong-cpp.github.io/cppwp/n4950/stmt.dcl"},
    },
    {
        .id = "raii.scope_cleanup",
        .skill_id = "raii.fundamentals",
        .title = "RAII и освобождение ресурса при выходе из области",
        .principle =
            "RAII связывает владение ресурсом со временем жизни объекта: успешно созданный объект хранит ресурс, а "
            "его деструктор освобождает ресурс.",
        .mechanism =
            "Автоматические объекты уничтожаются при выходе из области, в том числе при раскрутке стека из-за "
            "исключения. Уже построенные подобъекты также уничтожаются, если последующее построение завершилось "
            "исключением. Поэтому освобождение должно находиться в деструкторе владеющего объекта, а не только в "
            "последней строке обычного пути выполнения.",
        .example = "void update() {\n  std::lock_guard guard(mutex);\n  change_state();\n}\n// mutex будет освобождён "
                   "и при обычном выходе, и при исключении.",
        .common_mistake =
            "Получить ресурс вручную, а освобождать отдельным вызовом, до которого выполнение может не дойти из-за "
            "раннего return или исключения.",
        .sources = {"https://eel.is/c++draft/class.dtor", "https://eel.is/c++draft/except.ctor",
                    "https://eel.is/c++draft/thread.lock.guard"},
    },
    {
        .id = "lifetime.references",
        .skill_id = "lifetime.objects",
        .title = "Время жизни объекта, указатели и ссылки",
        .principle =
            "Указатель, ссылка или представление без владения не продлевают время жизни объекта, на который они "
            "указывают. После завершения времени жизни прежнего объекта такой доступ нельзя считать допустимым.",
        .mechanism =
            "Время жизни начинается и заканчивается по правилам конкретного объекта. Для некоторых привязок ссылки "
            "стандарт отдельно продлевает время жизни временного объекта, но это не общее свойство ссылок. В "
            "частности, возврат ссылки из функции не продлевает жизнь локального или временного объекта.",
        .example = "int copy_value(const int& value) {\n  return value;\n}\n\n"
                   "int prepare() {\n  int reading = 18;\n  return copy_value(reading);\n}\n"
                   "// copy_value заимствует живой reading только на время вызова.\n"
                   "// prepare возвращает самостоятельный int, а не ссылку на reading.",
        .common_mistake =
            "Рассуждать только о том, что адрес или значение ссылки сохранились, и не проверять, существует ли ещё "
            "сам объект.",
        .sources = {"https://timsong-cpp.github.io/cppwp/n4950/basic.life",
                    "https://timsong-cpp.github.io/cppwp/n4950/dcl.ref",
                    "https://timsong-cpp.github.io/cppwp/n4950/stmt.return",
                    "https://timsong-cpp.github.io/cppwp/n4950/class.temporary"},
    },
    {
        .id = "smart_pointers.ownership",
        .skill_id = "lifetime.smart_pointers",
        .title = "Владение и умные указатели",
        .principle =
            "Тип умного указателя выражает модель владения: std::unique_ptr — единственный владелец, std::shared_ptr "
            "— совместное владение через общий управляющий блок, std::weak_ptr — наблюдение без продления жизни.",
        .mechanism =
            "Перемещение std::unique_ptr передаёт владение. Копии std::shared_ptr разделяют один управляющий блок; "
            "создавать независимые shared_ptr из одного сырого указателя нельзя. std::weak_ptr::lock() позволяет "
            "атомарно получить временного совместного владельца, если объект ещё существует.",
        .example =
            "auto owner = std::make_shared<Widget>();\nstd::weak_ptr<Widget> observer = owner;\nif (auto value = "
            "observer.lock()) {\n  value->use();\n}",
        .common_mistake =
            "Выбирать shared_ptr просто для удобства передачи указателя или создавать несколько управляющих блоков "
            "для одного объекта.",
        .sources = {"https://eel.is/c++draft/unique.ptr", "https://eel.is/c++draft/util.smartptr.shared",
                    "https://eel.is/c++draft/util.smartptr.weak"},
    },
};

}  // namespace

std::span<const TheoryBlock> TheoryCatalog() {
  return kTheoryBlocks;
}

const TheoryBlock* FindTheoryForSkill(std::string_view skill_id) {
  const auto it = std::ranges::find(kTheoryBlocks, skill_id, &TheoryBlock::skill_id);
  return it == kTheoryBlocks.end() ? nullptr : &*it;
}

void ValidateTheoryCatalog() {
  std::unordered_set<std::string_view> ids;
  std::unordered_set<std::string_view> skills;
  for (const auto& block : kTheoryBlocks) {
    if (block.id.empty() || block.skill_id.empty() || block.title.empty() || block.principle.empty() ||
        block.mechanism.empty() || block.example.empty() || block.common_mistake.empty() || block.sources.empty()) {
      throw std::logic_error("theory block fields must not be empty");
    }
    if (FindSkill(block.skill_id) == nullptr) {
      throw std::logic_error("theory block references unknown skill: " + std::string(block.skill_id));
    }
    if (!ids.insert(block.id).second) {
      throw std::logic_error("duplicate theory block id: " + std::string(block.id));
    }
    if (!skills.insert(block.skill_id).second) {
      throw std::logic_error("a skill has more than one primary theory block: " + std::string(block.skill_id));
    }
    for (const auto source : block.sources) {
      const bool legacy_cpp =
          block.source_kind == LearningSourceKind::kCppStandard && source.starts_with("https://eel.is/c++draft/");
      if (!legacy_cpp && !IsLearningSource(source, block.source_kind)) {
        throw std::logic_error("theory must use a primary source for its subject: " + std::string(block.id));
      }
    }
  }

  const auto* pilot = FindLearningModule("lifetime_raii");
  if (pilot == nullptr) {
    throw std::logic_error("the lifetime and RAII pilot module is missing");
  }
  for (const auto skill_id : pilot->skill_ids) {
    if (FindTheoryForSkill(skill_id) == nullptr) {
      throw std::logic_error("the pilot module has no theory block for skill: " + std::string(skill_id));
    }
  }
}
