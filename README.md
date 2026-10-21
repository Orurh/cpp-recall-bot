# C++ Recall Bot

Telegram-тренажёр C++23 с адаптивной сложностью по темам, сессиями по 10 вопросов,
повтором ошибочных заданий и историей попыток в PostgreSQL.

## Запуск

Нужны CMake 3.28+, компилятор C++23, Boost.JSON/System, spdlog,
libcurl и клиентская библиотека PostgreSQL (`libpq`).

Создай `.env` с переменными `TELEGRAM_BOT_TOKEN` и `DATABASE_URL`.
Не добавляй `.env` в Git.

```bash
set -a
source .env
set +a
make run
```

Проверка содержимого:

```bash
make content-check
```

## Вопросы

JSON-карточки лежат в `content/questions`. Поддерживаются `single_choice`,
`multiple_choice`, `short_answer` и `matching`.

У `matching` от 2 до 6 пар вида:

```json
{
  "type": "matching",
  "pairs": [
    {"id": "sort", "left": "std::sort", "right": "Сортирует диапазон"},
    {"id": "find", "left": "std::find", "right": "Ищет равный элемент"}
  ]
}
```

Telegram показывает две перемешанные колонки. Можно начать выбор с любой стороны.
После правильной пары оба элемента исчезают. При ошибке счётчик ошибок
увеличивается, но карточка продолжается. После сопоставления всех пар
одна карточка считается одной попыткой: без ошибок — правильной, иначе
ошибочной, с повтором через существующую очередь обучения.

Если доступен GoogleTest, при конфигурации CMake включаются unit-тесты
`matching_round_tests`; запустить их можно `ctest --test-dir build --output-on-failure`.
