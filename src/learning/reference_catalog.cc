#include "reference_catalog.h"

#include "quiz/question.h"
#include "quiz/question_identity.h"

#include <boost/json.hpp>

#include <algorithm>
#include <stdexcept>
#include <unordered_set>
#include <utility>

namespace {

// Examples deliberately use other values than the cards. Help teaches the API,
// not the current answer. Bindings below are reviewed against a specific card
// revision. A shared container name/tag is NOT evidence of relevant help.
const std::vector<ReferenceEntry> kEntries{
    {"map_lookup",
     "std::map: найти значение и не вставить случайно",
     "Заголовок <map>. find(key) возвращает итератор на найденную пару или end(). "
     "contains(key), начиная с C++20, проверяет наличие ключа без вставки.",
     "У найденного итератора it->first — ключ, it->second — связанное значение. "
     "Перед разыменованием проверь it != end(). at(key) возвращает ссылку на значение "
     "или выбрасывает std::out_of_range, если ключа нет. operator[](key) при отсутствии "
     "ключа вставляет элемент; для std::map<Key, int> с обычным аллокатором его значение равно 0.",
     "std::map<int, int> scores{{4, 12}};\nauto it = scores.find(9);\n"
     "if (it != scores.end()) {\n  std::cout << it->second;\n}\n// Ключ 9 не добавлен; size() == 1.",
     "Проверять наличие через operator[] или считать итератор самим значением. "
     "Ключи map сравниваются по эквивалентности заданного порядка, не обязательно через ==.",
     {"https://timsong-cpp.github.io/cppwp/n4950/map.access",
      "https://timsong-cpp.github.io/cppwp/n4950/associative.reqmts"}},
    {"map_update",
     "std::map: вставить новое или обновить существующее",
     "С C++17 try_emplace(key, args...) вставляет элемент, только если ключа нет; "
     "существующее значение не заменяется. insert_or_assign(key, value) вставляет "
     "отсутствующий ключ или присваивает value существующему значению.",
     "Перегрузки без подсказки позиции возвращают pair<iterator, bool>: итератор "
     "указывает на элемент, bool сообщает, произошла ли вставка. У insert_or_assign "
     "false означает обновление, не ошибку. try_emplace не делает вычисление "
     "выражений аргументов ленивым: они вычисляются до входа в функцию.",
     "std::map<int, int> scores{{4, 12}};\nscores.try_emplace(4, 18); // осталось 12\n"
     "scores.insert_or_assign(4, 25); // стало 25; size() == 1",
     "Ожидать обновления от try_emplace, считать false признаком неудачи "
     "insert_or_assign или выбирать operator[] только ради проверки наличия ключа.",
     {"https://timsong-cpp.github.io/cppwp/n4950/map.modifiers",
      "https://timsong-cpp.github.io/cppwp/n4950/expr.call"}},
    {"vector_storage",
     "std::vector: размер, ёмкость и основные операции",
     "Заголовок <vector>. size() — число существующих элементов; capacity() — сколько элементов "
     "поместится без перевыделения хранилища. Это разные величины.",
     "reserve(n) не меняет size() и не создаёт элементы. resize(n, value) меняет размер: "
     "при росте добавляет копии value, при уменьшении удаляет хвост. operator[](i) в C++23 "
     "требует i < size(); at(i) при выходе за границы бросает std::out_of_range. "
     "capacity() не даёт права обращаться к несуществующим элементам. clear() уничтожает "
     "элементы, обнуляя size(), но сохраняет capacity(). shrink_to_fit() — необязательный "
     "запрос уменьшить ёмкость. push_back добавляет элемент в конец за амортизированное O(1); "
     "отдельное перевыделение занимает O(n). Вставка одного элемента в начало требует O(n).",
     "std::vector<int> numbers{4};\nnumbers.reserve(12); // size() == 1\n"
     "numbers.resize(3, 8); // элементы: 4, 8, 8\n// capacity() не обязана быть ровно 12.",
     "Принимать зарезервированную память за готовые элементы или считать capacity() точным результатом reserve(n).",
     {"https://timsong-cpp.github.io/cppwp/n4950/vector.capacity",
      "https://timsong-cpp.github.io/cppwp/n4950/sequence.reqmts",
      "https://timsong-cpp.github.io/cppwp/n4950/vector.overview",
      "https://timsong-cpp.github.io/cppwp/n4950/vector.modifiers"}},
    {"iterator_range",
     "Итератор и конец диапазона: когда можно читать элемент",
     "Диапазон [first, last) включает first и не включает last. У контейнера begin() "
     "указывает на первый элемент, если он есть; end() обозначает позицию после последнего.",
     "std::find из <algorithm> возвращает первый итератор на равный искомому элемент "
     "или переданный last, если совпадения нет. Перед *it проверь it != last. "
     "У пустого контейнера begin() == end(). Итератор — позиция, а не копия элемента; "
     "после изменения контейнера отдельно проверь, не стал ли он недействительным.",
     "std::vector<int> numbers{4, 8};\nauto it = std::find(numbers.begin(), numbers.end(), 8);\n"
     "if (it != numbers.end()) {\n  std::cout << *it; // 8\n}",
     "Разыменовывать end(), считать его последним элементом или путать итератор со значением элемента.",
     {"https://timsong-cpp.github.io/cppwp/n4950/iterator.requirements.general",
      "https://timsong-cpp.github.io/cppwp/n4950/alg.nonmodifying#alg.find"}},
    {"vector_iterators",
     "std::vector: что происходит с итераторами при изменении",
     "Инвалидация означает, что сохранённый итератор больше нельзя использовать как действительную позицию. "
     "Правила зависят от контейнера и операции; здесь речь только о vector.",
     "При перевыделении хранилища недействительны все прежние итераторы, указатели и ссылки на элементы, "
     "а также end(). Вставка без перевыделения сохраняет позиции до места вставки, но не на нём и после. "
     "erase инвалидирует итераторы и ссылки на удаляемой позиции и после неё. Он возвращает "
     "новый итератор на следующий элемент или новый end(), если следующего нет. "
     "Даже без перевыделения push_back меняет end(). clear() уничтожает все элементы "
     "и инвалидирует итераторы и ссылки на них, даже если ёмкость осталась прежней.",
     "std::vector<int> numbers{4, 8, 12};\nauto next = numbers.erase(numbers.begin());\n"
     "// next указывает на 8; если удалили последний элемент, получили бы end().",
     "Считать reserve защитой от любой инвалидации или продолжать обход старым итератором после erase.",
     {"https://timsong-cpp.github.io/cppwp/n4950/vector.capacity",
      "https://timsong-cpp.github.io/cppwp/n4950/vector.modifiers",
      "https://timsong-cpp.github.io/cppwp/n4950/sequence.reqmts"}},
    {"remove_erase",
     "std::remove и erase: перестановка и удаление — разные шаги",
     "std::remove и std::remove_if из <algorithm> сдвигают сохраняемые элементы к началу "
     "диапазона и возвращают его новый логический конец. Размер контейнера не меняется.",
     "Сохраняемые элементы идут в прежнем относительном порядке. За новым логическим концом "
     "остаются объекты в допустимом, но не определённом алгоритмом состоянии: их конкретные "
     "значения не нужно угадывать. Для vector вызов erase(new_end, end()) удаляет этот хвост. "
     "В C++20 и новее std::erase(vector, value) объединяет эти шаги.",
     "std::vector<int> numbers{4, 7, 4};\nauto tail = std::remove(numbers.begin(), numbers.end(), 4);\n"
     "// size() всё ещё 3; сохраняемый префикс содержит 7.\nnumbers.erase(tail, numbers.end()); // остался один "
     "элемент",
     "Считать, что алгоритм remove уже уменьшил size(), или ожидать определённых значений в хвосте.",
     {"https://timsong-cpp.github.io/cppwp/n4950/alg.remove",
      "https://timsong-cpp.github.io/cppwp/n4950/vector.erasure"}},
    {"object_lifetime",
     "Время жизни: адрес не является владельцем",
     "Сырой указатель хранит адрес, но сам по себе не управляет временем жизни объекта. "
     "Нужно отдельно проверить существование объекта в момент каждого чтения или записи.",
     "При выходе из блока заканчивается жизнь его автоматических объектов. Временный объект "
     "обычно уничтожается в конце полного выражения. При непосредственной привязке локальной "
     "ссылки const std::string& text = std::string{\"sample\"} временная строка живёт столько же, "
     "сколько эта ссылка. Но в C++23 временный объект в return функции, возвращающей ссылку, "
     "уничтожается в конце полного выражения return; ссылка у вызывающего его не спасает. "
     "Копирование адреса, string_view и получение указателя из метода сами жизнь владельца не продлевают.",
     "int number = 18;\nconst int* view = &number;\nnumber = 25;\n"
     "// Пока number жив, *view читает тот же объект и даст 25.\n"
     "// Копии указателя не являются отдельными копиями number.",
     "Считать, что сохранившийся адрес или прежнее содержимое памяти разрешают читать "
     "уже не существующий объект. Неопределённое поведение не означает обязательный сбой.",
     {"https://timsong-cpp.github.io/cppwp/n4950/basic.life",
      "https://timsong-cpp.github.io/cppwp/n4950/basic.stc.general",
      "https://timsong-cpp.github.io/cppwp/n4950/class.temporary"}},
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
      "https://timsong-cpp.github.io/cppwp/n4950/class.temporary"}},
    {"unique_ownership",
     "std::unique_ptr: владение и временный доступ",
     "Заголовок <memory>. unique_ptr выражает единоличное владение ресурсом. "
     "Перемещающий конструктор передаёт хранимый указатель новому владельцу, "
     "а исходный unique_ptr становится пустым.",
     "get() возвращает хранимый указатель без передачи владения. reset() заменяет "
     "его и вызывает удалитель для прежнего непустого указателя; с обычным удалителем "
     "это уничтожает прежний объект. release() отдаёт указатель без вызова удалителя. "
     "Для чтения без передачи владения можно принять const T& или const T*: выбор "
     "зависит от того, допускается ли отсутствие объекта. Передача unique_ptr по значению выражает владение. "
     "Копирование unique_ptr запрещено. Копия сырого указателя копирует адрес, а не сам объект: "
     "два независимо удаляющих его владельца привели бы к повторному удалению.",
     "auto owner = std::make_unique<int>(18);\nconst int& view = *owner;\n"
     "// view позволяет читать объект, но не становится его владельцем.\n"
     "// Функция void inspect(const int&) вызывается как inspect(*owner).",
     "Считать get() новым владельцем, использовать release() как удаление объекта "
     "или передавать unique_ptr по значению только ради чтения без намерения отдать ресурс.",
     {"https://timsong-cpp.github.io/cppwp/n4950/unique.ptr.single.ctor",
      "https://timsong-cpp.github.io/cppwp/n4950/unique.ptr.single.observers",
      "https://timsong-cpp.github.io/cppwp/n4950/unique.ptr.single.modifiers",
      "https://timsong-cpp.github.io/cppwp/n4950/unique.ptr.single",
      "https://timsong-cpp.github.io/cppwp/n4950/unique.ptr.dltr.dflt"}},
    {"scope_cleanup",
     "RAII: освобождение при выходе из области",
     "RAII связывает ресурс с владеющим объектом и его деструктором. "
     "При обычном выходе из блока или функции, включая return, уничтожаются "
     "созданные автоматические объекты покидаемых областей.",
     "При переходе от throw к подходящему обработчику идёт раскрутка стека: "
     "уничтожаются построенные объекты покидаемых областей, в обратном порядке построения. "
     "Объект снаружи покидаемого блока может оставаться живым. Например, деструктор "
     "lock_guard освобождает блокировку, но не подавляет исключение. Это не обещание "
     "очистки при любом аварийном завершении процесса. Поля класса разрушаются в порядке, "
     "обратном их объявлению. Если обычный, не делегирующий конструктор завершается исключением, "
     "уничтожаются уже построенные подобъекты, но не сам недостроенный объект и не поле, "
     "чья инициализация не завершилась.",
     "{\n  std::vector<int> buffer(24);\n  use(buffer);\n}\n"
     "// При обычном выходе из блока деструктор buffer освободит его хранилище.\n"
     "// Для чужого невладеющего указателя такое освобождение не выполняется.",
     "Вручную удалять внутреннее хранилище контейнера, забывать об уже построенных "
     "объектах на пути исключения или считать, что после catch уничтожены также все внешние объекты.",
     {"https://timsong-cpp.github.io/cppwp/n4950/stmt.dcl", "https://timsong-cpp.github.io/cppwp/n4950/stmt.jump",
      "https://timsong-cpp.github.io/cppwp/n4950/except.ctor", "https://timsong-cpp.github.io/cppwp/n4950/class.dtor",
      "https://timsong-cpp.github.io/cppwp/n4950/thread.lock.guard",
      "https://timsong-cpp.github.io/cppwp/n4950/vector.overview"}},
    {"object_slicing",
     "Срезка объекта: значение базового типа и ссылка — не одно и то же",
     "При обычном копировании Derived в отдельный объект Base копируется базовая часть. "
     "Производные поля и поведение не становятся частью нового Base. Это называют срезкой объекта.",
     "Параметр Base по значению и элемент vector<Base> — самостоятельные объекты Base. "
     "Ссылка Base& или const Base& может обозначать исходный Derived без копирования. "
     "Виртуальный вызов выбирает реализацию по фактическому объекту: у отдельной базовой копии "
     "это Base, у ссылки на исходный Derived — Derived. Исходный объект от копирования не меняется.",
     "struct Shape {\n  virtual int corners() const { return 0; }\n  virtual ~Shape() = default;\n};\n"
     "struct Triangle : Shape {\n  int corners() const override { return 3; }\n};\n"
     "Triangle original;\nShape copy = original;\nconst Shape& alias = original;\n"
     "// copy.corners() == 0; alias.corners() == 3.",
     "Считать, что virtual сохраняет производный тип при копировании по значению. "
     "Для полиморфного доступа нужна ссылка или указатель на существующий объект, "
     "с отдельно обеспеченным временем его жизни.",
     {"https://timsong-cpp.github.io/cppwp/n4950/class.copy.ctor",
      "https://timsong-cpp.github.io/cppwp/n4950/class.virtual"}},
    {"virtual_dispatch",
     "Виртуальный вызов: статический тип и фактический объект",
     "Статический тип известен при компиляции. Динамический тип — тип наиболее производного "
     "объекта, к которому относится ссылка или указатель в данном вызове.",
     "Для обычного виртуального вызова через базовую ссылку или указатель выбирается финальное "
     "переопределение для фактического объекта. override просит компилятор проверить, что "
     "функция действительно переопределяет виртуальную функцию базы. Невиртуальный метод "
     "выбирается по статическому типу выражения. Вызовы в конструкторах/деструкторах и "
     "явно квалифицированные вызовы имеют отдельные правила.",
     "struct Device {\n  virtual int code() const { return 4; }\n  virtual ~Device() = default;\n};\n"
     "struct Sensor : Device {\n  int code() const override { return 9; }\n};\n"
     "Sensor sensor;\nconst Device& device = sensor;\n// device.code() == 9: ссылка обозначает Sensor.",
     "Считать, что тип ссылки превращает исходный объект в базовый, или что одноимённая "
     "невиртуальная функция автоматически даёт динамический полиморфизм.",
     {"https://timsong-cpp.github.io/cppwp/n4950/class.virtual",
      "https://timsong-cpp.github.io/cppwp/n4950/class.cdtor"}},
    {"virtual_destruction",
     "Удаление через базовый тип: виртуальный деструктор",
     "При обычном delete через Base*, указывающий на объект Derived, базовому классу "
     "нужен виртуальный деструктор: тогда разрушается производный объект, затем его базовая часть.",
     "Без виртуального деструктора такое обычное удаление имеет неопределённое поведение, "
     "а не просто пропускает очистку Derived. unique_ptr<Base> с обычным удалителем "
     "тоже выполняет delete через базовый тип. Чисто виртуальному деструктору "
     "нужно определение: базовая часть всё равно разрушается. Формальное исключение из "
     "требования virtual в [expr.delete] — уничтожающий оператор удаления, который "
     "сам отвечает за разрушение; это не обычный delete из примера.",
     "struct Resource {\n  virtual ~Resource() = 0;\n};\nResource::~Resource() = default;\n"
     "struct File : Resource {};\nstd::unique_ptr<Resource> owner = std::make_unique<File>();\n"
     "// При уничтожении owner вызываются ~File(), затем ~Resource().",
     "Полагать, что другие виртуальные методы автоматически делают деструктор виртуальным, "
     "или что = 0 освобождает деструктор от необходимости иметь определение.",
     {"https://timsong-cpp.github.io/cppwp/n4950/expr.delete", "https://timsong-cpp.github.io/cppwp/n4950/class.dtor",
      "https://timsong-cpp.github.io/cppwp/n4950/unique.ptr.dltr.dflt"}},
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
      "https://timsong-cpp.github.io/cppwp/n4950/expr.sizeof"}},
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
      "https://timsong-cpp.github.io/cppwp/n4950/temp.variadic"}},
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
     {"https://timsong-cpp.github.io/cppwp/n4950/temp.variadic"}},
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
      "https://timsong-cpp.github.io/cppwp/n4950/func.require"}},
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
      "https://timsong-cpp.github.io/cppwp/n4950/func.require"}},
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
      "https://timsong-cpp.github.io/cppwp/n4950/meta.trans.cv"}},
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
      "https://timsong-cpp.github.io/cppwp/n4950/alg.binary.search"}},
    {"span",
     "std::span — представление без владения",
     "Заголовок <span>, C++20. std::span<T> представляет непрерывную последовательность чужих объектов. "
     "Он не копирует элементы и не продлевает их жизнь.",
     "std::span<const T> запрещает изменение элементов через представление. const std::span<T> "
     "делает константным сам объект представления, но не T. В C++23 operator[] требует индекс меньше size().",
     "int values[]{4, 8};\nstd::span<const int> view = values;\nvalues[1] = 12;\n// view[1] теперь 12; данные общие",
     "Возвращать span на локальный массив, использовать его после разрушения владельца или "
     "перевыделения буфера vector. Константный доступ не означает отдельную неизменяемую копию.",
     {"https://timsong-cpp.github.io/cppwp/n4950/views.span", "https://timsong-cpp.github.io/cppwp/n4950/span.elem"}},
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
      "https://timsong-cpp.github.io/cppwp/n4950/comparisons"}},
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
      "https://timsong-cpp.github.io/cppwp/n4950/alg.sorting.general"}},
};

// Updating a card requires rechecking its help, not automatically following its
// tags. See docs/reviews/references/2026-10-26-bindings.json for the reviewed matrix.
const std::vector<ReferenceBinding> kBindings{
    {"binary_fold_empty_pack_initial_value",
     "1a432295f866935731b5daef769b389e08124c0517796b9f7c25ed2588ee6390",
     {"parameter_packs", "fold_expressions", "empty_fold"}},
    {"c_str_from_temporary_dangles",
     "5dd099e02d979d28f2ef161837b0f7395257f5ae19886afb43ab85f0fd46877b",
     {"object_lifetime", "string_c_str"}},
    {"const_reference_extends_temporary",
     "24388f91d8e85b5474c38723110ad52331a877f6c933d52cac7da90107a075b2",
     {"object_lifetime"}},
    {"erase_remove_vector_size", "51e7c587f3e7d9db4525654e5aaa57db7c7d02e1b502e2557a6eb7778b41cfa4", {"remove_erase"}},
    {"fold_and_empty_pack_identity",
     "4b27642fb21a100e4c37dc6736310ee93844cd2749975d0beb4cb521d2eecfd9",
     {"parameter_packs", "fold_expressions", "empty_fold"}},
    {"function_parameter_address_dangles",
     "45a16a4cff92cf3ae2a6b9f8c11caa458f4393e22183bce79c0e9bb3190f4dc2",
     {"object_lifetime"}},
    {"lower_bound_partition_precondition",
     "f74764670b450cf8f2c24f88369ab895f0e487619c601f0745c51f59aeab2b91",
     {"lower_bound"}},
    {"map_lookup_methods_matching", "43bcb8cadc89ece61d04b5a6a8ead4cf99d0198dca76ea3440fcc32ce157e9c1", {"map_lookup"}},
    {"map_subscript_inserts_default_value",
     "40578e057ccc14a0bad80fd8136578299222f7dbaee16e599a136bc8700e024f",
     {"map_lookup"}},
    {"oop_object_slicing_definition",
     "47ef98483a3a3d34cb6457d0af743c0fef3bdc4155a307c732b88b5c95ab4382",
     {"object_slicing"}},
    {"oop_pure_virtual_destructor_definition",
     "fcd969ad9663da4bed45ab0084db3fc47ddb1b4a1cd2c0f67df6ac1dab6a4e4a",
     {"virtual_destruction"}},
    {"oop_slicing_value_vs_reference",
     "afa3568b0ab14a83ff06c15b4045ec1ebbb3b880de1d916c7decc5f05981ab3d",
     {"object_slicing"}},
    {"oop_vector_base_slices_derived",
     "575bd3c7ed518c2bd4f07f855ff2d2fd96152fd681f615f45c33b9f802e3b2a5",
     {"object_slicing"}},
    {"oop_virtual_destructor_reason",
     "35740c27c496bb11c356932ea1db9a486ce198843bad90e75ca356f55f16a1c6",
     {"virtual_destruction"}},
    {"oop_virtual_dispatch_basic",
     "f87089c4c51694005a28a8f8956ce23beaa4098ae61aaf7da5dffdd68813c7c2",
     {"virtual_dispatch"}},
    {"oop_virtual_function_definition",
     "ec30d0d74bded400974547f31dbad6a7dbbb80dfbeb6234889cdd12dfd3e7172",
     {"virtual_dispatch"}},
    {"pointer_after_local_lifetime",
     "239bbdd4d04246ad2b2ad1ed67bffc80bce6c3bdad763ebe533893da85743bce",
     {"object_lifetime"}},
    {"raii_closes_file_on_early_exit",
     "ec059b256728a9f4f8d68b42ecf20479bb83ab0d6ad70acd311c6e845fabbbbf",
     {"scope_cleanup"}},
    {"raii_definition", "4baeab2232ab4c788242bb6109dc4050c781f4a8e24e56889857f1de1f28e6fc", {"scope_cleanup"}},
    {"raii_lock_guard_unlocks_during_unwinding",
     "0308a63db240cc919c7271f4ac1577798d8b5afccf6eba5ec0513c4f8bdc2448",
     {"scope_cleanup"}},
    {"raii_member_destruction_dependency",
     "103a10a6d108e6b2248b9339dff64313f0a4c5234330f6b1daec84d8c277200a",
     {"scope_cleanup"}},
    {"raii_partial_construction_cleanup",
     "f632199814dcba76ca4227a040876d4ffe1ad4502beba121dc4f529b96e8568d",
     {"scope_cleanup"}},
    {"raii_raw_owner_copy_double_delete",
     "77c7ded1c384c5bac933368d5e11b8fcd9cc4dd7ea322d23aee72b1129d48b6c",
     {"unique_ownership"}},
    {"raii_reverse_local_destruction_order",
     "0131156a427b81df2510829b62b5e17ac01506f3f4ec0d573e5cc0aff4a24d8e",
     {"scope_cleanup"}},
    {"return_reference_does_not_extend_temporary",
     "ea579816ffac681d53135a006554de399ff0622beaa358f6b713386fbffc0cd3",
     {"object_lifetime"}},
    {"smart_pointers_matching",
     "c010d9e6c91c02e507766883ece859b98b7b167f8714df7e6b6b7ac9ea5cf656",
     {"unique_ownership"}},
    {"span_after_vector_reallocation",
     "e2006cfdfc879fa9c7476b123209be3907c47956b356c631ab4765ce4eeb9bce",
     {"vector_iterators", "span"}},
    {"stl_algorithms_matching",
     "eee43c8922ef9480172dea9d46fb0a7495fd33ed4154fc9ad7ddc6b2eb0a7a51",
     {"iterator_range", "remove_erase", "lower_bound"}},
    {"string_view_from_temporary_string",
     "470c30e637beae03cf4e7ea9b0c3f7045f631bc742289f162d501a6f4cfa2cde",
     {"object_lifetime"}},
    {"unique_ptr_borrow_without_transfer",
     "22377f9a6676e819df655f3207fd43a3ea8e775bd9de7790c54cb6e194649e92",
     {"unique_ownership"}},
    {"unique_ptr_move_transfers_ownership",
     "c83486b46c8287a4a13c8a491525e21e9fe1a54e7533823121c2903f6fe081eb",
     {"unique_ownership"}},
    {"unique_ptr_scope_deletes_object",
     "f3fcae3481d2be13cfd3c833d6781a72122d3fd9caaf01a9306e4868fad5c7cc",
     {"unique_ownership"}},
    {"vector_access_complexity_matching",
     "460d27ba9a779622786adb9170d702e085385655fc89989245772457c7071a0d",
     {"vector_storage"}},
    {"vector_invalidation_multiple_choice",
     "662959b3f399f96b25610ad12d62d517221066ab54fa57be2449a7fd5ffd2ccf",
     {"vector_iterators"}},
    {"vector_methods_matching", "03c9f4d71ee8308dc5c6ebbfb18922fd602bcc6d832817143a329bad8709c2b4", {"vector_storage"}},
    {"vector_raii_early_return", "51faaff2fd8a259b9b577c01349dcd01e9f277f424d1c2edf12c47fc32ba1522", {"scope_cleanup"}},
    {"vector_reserve_does_not_change_size",
     "fd8cba3805b2077a08725c428b650665895fc7f9b51dee531fd0d96e043cce38",
     {"vector_storage"}},
    {"invoke_member_data_write", "5ddb874db35e828253e057956e251242120e0daac74a6c1097740a43adb5980c", {"invoke"}},
    {"invoke_const_member_function", "51a7f88576c19017252c95f16622ea50e0afeceb4f2bf74b9f4d68c8b9935a01", {"invoke"}},
    {"invoke_result_const_member_type",
     "0be4302612d8d0da1441aefb6932746af80937105a3075a286ef392b3ad02079",
     {"invoke", "invoke_result"}},
    {"remove_cvref_const_reference",
     "f80df269d6d2e584363ccbcd3a3a36d8f41ed15413dd019d99809747ee36f4c6",
     {"remove_cvref"}},
    {"remove_cvref_keeps_pointee_const",
     "dcdea88d186aaee7e7a836ed1df9cbba635e0b11400b2753dd2543383e5b6793",
     {"remove_cvref"}},
    {"lower_bound_first_equal_position",
     "1ac4e4883dceccec8e0b5a7d94247d28db4693bdee9f68fef98a23da0dd1a010",
     {"lower_bound"}},
    {"lower_bound_descending_boundary",
     "7a430f3652bfac9fcbe7097086f965cf470e05a55e9844b801a7359f19c5e258",
     {"lower_bound", "ordering"}},
    {"lower_bound_candidate_not_match",
     "31e0fdaf899ff13832610b9caa370fa1cd58fda8857580e1141a7a43a68f2e15",
     {"lower_bound"}},
    {"span_observes_owner_changes", "c5b464eed67ae2fa647c4d90453bc1b9cb023bc40f34661588e2edb319fa3ae5", {"span"}},
    {"const_span_mutable_elements", "642c666c60dc778c189dcaf5fbad34f452d51585ff2c8e143e010015fb3be68e", {"span"}},
    {"comparator_equivalence_not_equality",
     "831621b5e8c8e6236c2904571224c3b97f7461b9ef89084e8b02cfbc515b0eb8",
     {"ordering"}},
    {"ranges_sort_member_projection",
     "39d197b6697d33097af5325f027846aa41562d83e7d5502c704fdf3e01492a95",
     {"ordering", "sort_projection"}},
    {"parameter_pack_element_count",
     "6c74b88fbd5a99f18fcd13e02b9ff8c83316a59a1ac41afb2054e99450ce6fa3",
     {"parameter_packs"}},
    {"parameter_pack_expand_call",
     "1b07337c3ffed6ae3160aecb01f9621d24582561152c7bb1c2a58516e5e07182",
     {"parameter_packs"}},
    {"fold_and_nonempty_pack",
     "74e5b8867a2bfefa3fe7e2dc7af94adfa3df0cef3419ccc38a1a1ba323cfceeb",
     {"parameter_packs", "fold_expressions"}},
    {"fold_subtraction_grouping",
     "0807712df0b31c7a4868f59189361511fa64b32649f6a453f02ff3ea6b785f00",
     {"parameter_packs", "fold_expressions"}},
    {"sorted_snapshot_key_alias_completion",
     "6b98e382b9c275850f7f56478a8ffafde5a665f9075a930a01f990b675ae0d43",
     {"invoke", "invoke_result", "remove_cvref"}},
    {"vector_resize_adds_elements",
     "d1067fe8796703837fcf141806d052a762e31817cb1505b776170f1337f34455",
     {"vector_storage"}},
    {"find_missing_end_guard", "9d82ab2fc17a354883c34deb2220791fdb1d0f867e22775ec437302c9bcbf167", {"iterator_range"}},
    {"vector_erase_return_next",
     "67a6104e6707987331be452117e11fbeeb2948fda7360bd28bb1151f886aab98",
     {"iterator_range", "vector_iterators"}},
    {"vector_erase_filter_loop",
     "39b4db1dec6483af45bf36b06fe5b8367077df48547918e45e44ac4be18a2f6c",
     {"iterator_range", "vector_iterators"}},
    {"map_find_without_insertion",
     "16bb4ee89350743b8acb63e0737c93465db266508d9be0b06237c9775450d569",
     {"map_lookup", "iterator_range"}},
    {"map_insert_or_assign_vs_try_emplace",
     "d3351c099b94e43f618ee454c20415bb3c998a5193bd9d25474024788f403889",
     {"map_update"}},
    {"map_update_existing_stock",
     "249ff494d5e5020ab6874bf53ba87f64005dd14fe6b333b17ec645d06f57a9da",
     {"map_lookup", "map_update"}},
};

}  // namespace

std::span<const ReferenceEntry> ReferenceCatalog() {
  return kEntries;
}

std::span<const ReferenceBinding> ReferenceBindingCatalog() {
  return kBindings;
}

std::string ReferenceContentRevision(const ReferenceEntry& entry) {
  boost::json::array sources;
  for (const auto source : entry.sources) {
    sources.emplace_back(source);
  }
  const boost::json::object content{{"id", entry.id},
                                    {"title", entry.title},
                                    {"principle", entry.principle},
                                    {"mechanism", entry.mechanism},
                                    {"example", entry.example},
                                    {"common_mistake", entry.common_mistake},
                                    {"sources", std::move(sources)}};
  return QuestionContentRevision(CanonicalQuestionJson(content));
}

void ValidateReferenceCatalog(std::span<const Question> questions) {
  std::unordered_set<std::string_view> reference_ids;
  for (const auto& entry : kEntries) {
    if (entry.id.empty() || !reference_ids.insert(entry.id).second || entry.title.empty() || entry.principle.empty() ||
        entry.mechanism.empty() || entry.example.empty() || entry.common_mistake.empty() || entry.sources.empty()) {
      throw std::invalid_argument("incomplete or duplicate reference: " + std::string(entry.id));
    }
    for (const auto source : entry.sources) {
      if (!source.starts_with("https://timsong-cpp.github.io/cppwp/n4950/")) {
        throw std::invalid_argument("reference must identify its C++23 source: " + std::string(entry.id));
      }
    }
  }
  std::unordered_set<std::string_view> question_ids;
  for (const auto& binding : kBindings) {
    const auto question = std::ranges::find(questions, binding.question_id, &Question::id);
    if (!question_ids.insert(binding.question_id).second || question == questions.end() ||
        binding.content_revision != question->content_revision || binding.content_revision.empty() ||
        binding.reference_ids.empty()) {
      throw std::invalid_argument("missing, stale or duplicate reference binding: " + std::string(binding.question_id));
    }
    std::unordered_set<std::string_view> seen;
    for (const auto id : binding.reference_ids) {
      if (!reference_ids.contains(id) || !seen.insert(id).second) {
        throw std::invalid_argument("unknown or duplicate reference in binding: " + std::string(binding.question_id));
      }
    }
  }
}

std::vector<const ReferenceEntry*> ReferencesForQuestion(const Question& question) {
  std::vector<const ReferenceEntry*> result;
  const auto binding = std::ranges::find(kBindings, question.id, &ReferenceBinding::question_id);
  // Existing/synthetic sessions must fail closed if a card changed after review.
  if (binding == kBindings.end() || binding->content_revision != question.content_revision) {
    return result;
  }
  for (const auto id : binding->reference_ids) {
    const auto entry = std::ranges::find(kEntries, id, &ReferenceEntry::id);
    if (entry == kEntries.end()) {
      return {};
    }
    result.push_back(&*entry);
  }
  return result;
}
