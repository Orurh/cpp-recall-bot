(function () {
  'use strict';

  const MAX_CODE_LENGTH = 16384;
  const compileCommand = 'g++ -std=c++23 -Wall -Wextra -pedantic take_prefix.cpp -o take_prefix && ./take_prefix';
  const headers = '#include <algorithm>\n#include <array>\n#include <cstddef>\n#include <iostream>\n#include <limits>\n#include <vector>\n';
  const signature = 'std::vector<int> TakePrefix(std::vector<int>& pending, std::size_t limit)';
  const testsSource = `
namespace coding_practice_tests {
bool CheckCase(std::vector<int> pending, std::size_t limit,
               const std::vector<int>& expected_result,
               const std::vector<int>& expected_pending, const char* name) {
  const auto result = TakePrefix(pending, limit);
  if (result != expected_result || pending != expected_pending) {
    std::cerr << "Не выполнен случай: " << name << '\\n';
    return false;
  }
  return true;
}
}  // namespace coding_practice_tests

int main() {
  using coding_practice_tests::CheckCase;
  bool ok = true;
  ok = CheckCase({}, 0, {}, {}, "пустой ввод, ноль") && ok;
  ok = CheckCase({}, 5, {}, {}, "пустой ввод, положительный предел") && ok;
  ok = CheckCase({4, 7, 4, 9}, 0, {}, {4, 7, 4, 9}, "нулевой предел") && ok;
  ok = CheckCase({4, 7, 4, 9}, 2, {4, 7}, {4, 9}, "часть последовательности") && ok;
  ok = CheckCase({4, 7, 4, 9}, 4, {4, 7, 4, 9}, {}, "точный размер") && ok;
  ok = CheckCase({4, 7, 4, 9}, 99, {4, 7, 4, 9}, {}, "предел больше размера") && ok;
  ok = CheckCase({8}, 1, {8}, {}, "один элемент") && ok;
  ok = CheckCase({4, 7, 4, 9}, std::numeric_limits<std::size_t>::max(),
                 {4, 7, 4, 9}, {}, "максимальный std::size_t") && ok;
  ok = CheckCase({6, 6, -2, -2, 8}, 3, {6, 6, -2}, {-2, 8}, "повторы и порядок") && ok;
  ok = CheckCase({std::numeric_limits<int>::min(), 0, std::numeric_limits<int>::max()},
                 2, {std::numeric_limits<int>::min(), 0},
                 {std::numeric_limits<int>::max()}, "крайние значения int") && ok;
  std::vector<int> pending{3, 1, 3, 2, 5};
  const auto first = TakePrefix(pending, 2);
  const auto second = TakePrefix(pending, 2);
  const auto third = TakePrefix(pending, 2);
  const auto fourth = TakePrefix(pending, 2);
  if (first != std::vector<int>{3, 1} || second != std::vector<int>{3, 2} ||
      third != std::vector<int>{5} || !fourth.empty() || !pending.empty()) {
    std::cerr << "Не выполнен случай: последовательные вызовы\\n";
    ok = false;
  }
  if (!ok) return 1;
  std::cout << "Открытые тесты пройдены. Это не доказательство корректности для всех входов.\\n";
}
`;

  const practice = {
    id: 'take_prefix',
    revision: 1,
    MAX_CODE_LENGTH,
    title: 'CF-01 · Отделить начало вектора',
    summary: 'Напиши функцию целиком, без main(). Верни первые limit элементов pending (если их меньше — все) и удали их из исходного вектора. Сохрани порядок и повторы.',
    prompt: 'Напишите только указанную функцию на C++23, без main(). Она возвращает первые limit элементов pending (если элементов меньше — все) и удаляет именно их из pending. Сохраните исходный порядок и все повторы в результате и остатке. При limit == 0 или пустом pending верните пустой вектор, не меняя pending. Если limit не меньше исходного размера, верните все элементы, оставив pending пустым. Допустим любой limit типа std::size_t. Требования относятся к успешному возврату; ёмкость и обработка ошибок выделения памяти не проверяются. Самопроверка в своей среде: приложение код не запускает и не оценивает, XP и подтверждения навыка нет.',
    signature,
    starter: '#include <cstddef>\n#include <vector>\n\n' + signature + ' {\n  // TODO: напишите реализацию.\n}\n',
    prerequisites: [
      'Функция с возвращаемым значением; параметр по ссылке и изменение исходного объекта.',
      'std::vector<int>, size(), begin(), end() и полуоткрытый диапазон [first, last).',
      'Ветвление или цикл; недопустимость обращения за концом диапазона; инвалидация после erase.'
    ],
    examples: [
      { input: 'pending = {4, 7, 4, 9}, limit = 2', output: '{4, 7}', remaining: '{4, 9}' },
      { input: 'pending = {4, 7, 4, 9}, limit = 0', output: '{}', remaining: '{4, 7, 4, 9}' },
      { input: 'pending = {8}, limit = 99', output: '{8}', remaining: '{}' },
      { input: 'pending = {}, limit = 5', output: '{}', remaining: '{}' }
    ],
    help: {
      title: 'Диапазоны, копия и удаление в std::vector',
      paragraphs: [
        'Диапазон [first, last) включает first и не включает last. При first == last он пуст. begin() == end() у пустого контейнера; end() нельзя разыменовывать. Границы передаваемого библиотеке диапазона должны быть действительными.',
        'Конструктор std::vector(first, last) создаёт отдельный вектор из элементов указанного диапазона в том же порядке. Пример ниже копирует целый фиксированный массив; последующее изменение int в исходном массиве не меняет копию.',
        'Метод vector::erase(first, last) удаляет элементы действительного полуоткрытого диапазона. После удаления итераторы и ссылки на удаляемой позиции и после неё недействительны. Ёмкость и число существующих элементов — разные величины: reserve не создаёт элементы.',
        'Не вычисляйте позицию за end() и не используйте старые позиции после их инвалидации. Открытые тесты проверяют конкретные результаты; безопасность всех обращений нужно проверить по самому коду.'
      ],
      code: '#include <array>\n#include <vector>\n\nint main() {\n  std::array<int, 3> source{6, 2, 6};\n  std::vector<int> snapshot(source.begin(), source.end());\n  source[0] = 9;\n  // snapshot содержит 6, 2, 6: это отдельные объекты int.\n}\n',
      sources: [
        { title: 'N4950: конструктор vector из диапазона, [vector.cons]/9', url: 'https://timsong-cpp.github.io/cppwp/n4950/vector.cons#9' },
        { title: 'N4950: удаление диапазона, [sequence.reqmts]/49–52', url: 'https://timsong-cpp.github.io/cppwp/n4950/sequence.reqmts#49' },
        { title: 'N4950: инвалидация после erase, [vector.modifiers]/3', url: 'https://timsong-cpp.github.io/cppwp/n4950/vector.modifiers#3' },
        { title: 'N4950: диапазоны и конец, [iterator.requirements.general]/7–12', url: 'https://timsong-cpp.github.io/cppwp/n4950/iterator.requirements.general#7' },
        { title: 'N4950: размер и reserve, [vector.capacity]', url: 'https://timsong-cpp.github.io/cppwp/n4950/vector.capacity' }
      ]
    },
    solution: signature + ' {\n  auto split = pending.begin();\n  std::size_t taken = 0;\n  while (taken < limit && split != pending.end()) {\n    ++split;\n    ++taken;\n  }\n  std::vector<int> result(pending.begin(), split);\n  pending.erase(pending.begin(), split);\n  return result;\n}\n',
    explanation: [
      'Один возможный способ: найти границу, останавливаясь по пределу или по end(). Даже максимальный limit не заставляет вычислять позицию за концом. Другие корректные реализации тоже допустимы.',
      'Копия выбранного диапазона создаётся до изменения pending. Затем удаляется тот же диапазон. Старый split после erase больше не используется.',
      'При нуле или пустом вводе границы совпадают: результат пуст, удаляемый диапазон пуст. При большом пределе граница доходит до end(): возвращаются все элементы, pending становится пустым.',
      'Можно иначе скопировать элементы циклом, использовать стандартные алгоритмы или построить результат и остаток отдельно. Рубрика проверяет контракт и допустимость операций, а не совпадение с эталоном. Успех открытых тестов сам по себе не исключает ошибки на других входах.'
    ],
    rubric: [
      'Функция с указанной сигнатурой компилируется как C++23; выбранный способ не ограничен эталоном.',
      'Возвращены ровно min(limit, исходный размер pending) первых элементов, включая повторы и исходный порядок.',
      'В pending остался ровно исходный суффикс в исходном порядке; остальные элементы не потеряны и не добавлены.',
      'Проверены пустой ввод, нулевой предел, один элемент, точный размер, превышение размера и максимальный std::size_t.',
      'Несколько последовательных вызовов правильно работают с оставшимся pending.',
      'По коду проверено отсутствие недопустимых диапазонов, разыменования end() и повторного использования инвалидированных позиций. Открытые тесты этого не доказывают.',
      'Отдельно отметьте первый самостоятельный код, использование справки или эталона и исправления после диагностики. Сохранение текста и самопроверка не означают автоматическую оценку, XP или подтверждение навыка.'
    ],
    compileCommand,
    testsSource,
    buildSource(code) {
      if (typeof code !== 'string') throw new TypeError('Код должен быть строкой.');
      if (new TextEncoder().encode(code).length > MAX_CODE_LENGTH) throw new RangeError('Код длиннее 16384 байт UTF-8.');
      return '// CF-01: сохраните файл как take_prefix.cpp и запустите в своей среде.\n' +
        '// ' + compileCommand + '\n' + headers + '\n' + code + '\n' + testsSource;
    }
  };

  function freezeConstants(value) {
    if (value && typeof value === 'object') {
      Object.values(value).forEach(freezeConstants);
      Object.freeze(value);
    }
    return value;
  }
  globalThis.CppRecallCodingPractice = freezeConstants(practice);
}());
