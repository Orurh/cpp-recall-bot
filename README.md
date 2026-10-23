# C++ Recall Bot

Telegram-тренажёр для повторения C++. Бот подбирает сложность отдельно по темам,
возвращает ошибки в очередь до трёх попыток и планирует последующие повторения по
истории ответов. Попытки и прогресс хранятся в PostgreSQL.

## Возможности

- режимы «Теория», «API / STL» и «Код»;
- выбор одной, нескольких или всех тем перед тренировкой;
- выбор любого открытого уровня в Mini App; более сложные уровни заблокированы;
- одиночный, множественный, короткий ответ и сопоставление;
- адаптивный уровень пользователя и каждой темы;
- каталог микроскиллов с проверяемыми зависимостями и привязкой каждого вопроса;
- интервалы повторения 1, 3, 7, 14 и 30 дней;
- атомарная и идемпотентная запись ответа;
- статистика по темам, точности и повторениям по команде `/stats`;
- 155 вопросов в JSON с автоматической проверкой структуры;
- первый учебный модуль «Lifetime и RAII»: 40 вопросов по четырём связанным навыкам;
- 42 вопроса по ООП: от объектов и инкапсуляции до статического и динамического полиморфизма;
- отдельные блоки по API/STL пятого уровня, шаблонам, атомикам и async/future;
- отдельный прогресс по навыкам, не смешанный с адаптивной сложностью.

### Карточки сопоставления

JSON-карточка `matching` содержит от 2 до 6 пар:

```json
{
  "type": "matching",
  "pairs": [
    {"id": "sort", "left": "std::sort", "right": "Сортирует диапазон"},
    {"id": "find", "left": "std::find", "right": "Ищет равный элемент"}
  ]
}
```

Mini App показывает две перемешанные колонки. После правильной пары оба элемента
исчезают; ошибка увеличивает счётчик, но не завершает карточку.

## Быстрый запуск

Нужны CMake 3.28+, компилятор с C++23, PostgreSQL и библиотеки Boost.JSON,
Boost.System, libcurl, libpq и spdlog.

```bash
cp .env.example .env
# Укажите TELEGRAM_BOT_TOKEN и замените пароль в .env.
docker compose up -d postgres
make run
```

`make run` загружает переменные из `.env`. При старте приложение автоматически
применяет SQL-файлы из каталога `migrations/`.

Для запуска приложения и базы целиком в контейнерах:

```bash
cp .env.example .env
# Заполните TELEGRAM_BOT_TOKEN и POSTGRES_PASSWORD.
docker compose up --build -d
docker compose logs -f bot
```

Порт PostgreSQL публикуется только на `127.0.0.1`. Если `5432` занят
другим проектом, задай в `.env`, например, `POSTGRES_HOST_PORT=5433`.

## Telegram Mini App

Полная тренировка доступна в Mini App: выбор уровня и тем, все типы вопросов, серверная
проверка ответов и синхронизированный PostgreSQL-прогресс. При заданном `MINI_APP_URL` бот
автоматически настраивает кнопку меню. Команды `/start` и `/menu` также отправляют Web App-кнопку.
Прохождение карточек непосредственно в чате не поддерживается.

Настройка HTTPS, reverse proxy и BotFather: [`docs/MINIAPP_SETUP.md`](docs/MINIAPP_SETUP.md).

Для временной end-to-end проверки через GitHub Codespaces добавь Codespaces secret
`TELEGRAM_BOT_TOKEN`, создай Codespace и выполни:

```bash
./scripts/run_codespace.sh
```

Скрипт вычислит публичный HTTPS URL, запустит Compose и попытается открыть порт
8080. Codespaces переводит публичные порты обратно в private после перезапуска, поэтому скрипт
нужно запускать заново при каждом старте Codespace.

## Конфигурация

| Переменная | Назначение |
| --- | --- |
| `TELEGRAM_BOT_TOKEN` | токен бота от BotFather |
| `DATABASE_URL` | PostgreSQL URI для локального запуска |
| `POSTGRES_DB` | имя базы в Docker Compose |
| `POSTGRES_USER` | пользователь базы в Docker Compose |
| `POSTGRES_PASSWORD` | пароль базы в Docker Compose |

## Проверки

```bash
make check          # сборка, контент, unit-тесты, формат и cppcheck
make test           # CTest; storage-тест пропускается без TEST_DATABASE_URL
make content-check  # проверка всех JSON-вопросов
make format         # применить clang-format
make tidy           # отдельная строгая проверка clang-tidy
```

Интеграционный тест PostgreSQL можно запустить так:

```bash
TEST_DATABASE_URL="$DATABASE_URL" ctest --test-dir build \
  --output-on-failure -R storage_tests
```

## Структура

- `src/app` — сценарий тренировки и адаптивный выбор;
- `src/bot` — Telegram long polling, Web App-точка входа и статистика;
- `src/learning` — очередь повторных попыток и состояние повторения;
- `src/storage` — загрузка контента и PostgreSQL;
- `content/questions` — банк вопросов;
- `migrations` — версионируемая схема базы;
- `tests` — unit- и интеграционные тесты.

Каждый вопрос обязан содержать `id`, `topic`, `kind`, `level`, `standard`,
непустой `tags`, `text`, `task` и `explanation`. Команда `make content-check`
проверяет тему по каталогу, тип задания, варианты ответа, диапазон уровня и
уникальность `id`. Каталог тем и их пользовательские названия находится в
`src/quiz/topic_catalog.h`.

Правила попыток, адаптивного уровня, интервальных повторений и метрик описаны в
[`docs/training_methodology.md`](docs/training_methodology.md).
Порядок следующих учебных функций зафиксирован в
[`docs/LEARNING_ROADMAP.md`](docs/LEARNING_ROADMAP.md).
