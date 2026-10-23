# C++ Recall — мини-приложение Telegram (Mini App)

Мини-приложение теперь ведёт всю тренировку: выбор открытой сложности, форматов и тем,
одиночные, множественные и короткие ответы, сопоставление, разбор и итоги.
Сервер C++ остаётся единственным источником истины: он выбирает вопросы, проверяет ответы
и записывает прогресс в PostgreSQL. Ключи ответов в браузер не отдаются.

Сервер не примет уровень выше `unlocked_level`. Повторение более простого уровня обновляет
статистику и интервалы повторения, но не изменяет адаптивное свидетельство. Повышение может дать только тренировка
на самом высоком открытом уровне.

## Как это устроено

- `docs/miniapp/` — HTML/CSS/JS без Node-сборки;
- `src/miniapp/miniapp_http_server.*` — HTTP и статика на Boost.Beast;
- `src/miniapp/telegram_init_data.*` — HMAC-SHA-256 проверка `Telegram.WebApp.initData`;
- `src/miniapp/miniapp_service.*` — сессия, адаптивная очередь и запись прогресса.

Сервер отдаёт обычный HTTP. Перед ним нужен обратный прокси (reverse proxy) с TLS; Telegram требует публичный
HTTPS URL. GitHub Pages больше не подходит: мини-приложение вызывает API на том же домене.

## Переменные

Добавь в `.env`:

```env
MINI_APP_URL=https://learn.example.com/
MINI_APP_LISTEN_ADDRESS=127.0.0.1
MINI_APP_PORT=8080
MINI_APP_STATIC_DIR=docs/miniapp
MINI_APP_AUTH_MAX_AGE_SECONDS=86400
```

`MINI_APP_URL` включает HTTP-сервер и автоматическую кнопку меню Telegram. Без неё тренировки
недоступны: бот сообщит об этом по команде `/start` или `/menu`. URL может иметь путь,
например `https://example.com/recall/`.

## Обратный HTTPS-прокси

Пример Caddy для запуска бота на host-машине:

```caddyfile
learn.example.com {
    reverse_proxy 127.0.0.1:8080
}
```

При Docker Compose порт бота уже публикуется только на `127.0.0.1`. Проверь после запуска:

```sh
curl --fail http://127.0.0.1:8080/
curl --fail https://learn.example.com/
```

## Совместный хост с other services

На сервере other services доступ к Telegram API идёт через Xray в Docker-сети
`xray_default`. Запускай C++ Recall с дополнительным Compose-файлом:

```sh
docker compose -f compose.yaml -f deploy/outbound-proxy.compose.yaml up -d --build
```

Профиль подключает только контейнер бота к готовому HTTP-прокси; базы
двух проектов и их Compose-сети остаются разделены.

Для публичного HTTPS добавь в `.env` хост без схемы и URL со схемой:

```env
MINI_APP_DOMAIN=learn.example.com
MINI_APP_URL=https://learn.example.com/
```

Затем запусти Caddy вместе с ботом:

```sh
docker compose \
  -f compose.yaml \
  -f deploy/outbound-proxy.compose.yaml \
  -f deploy/public.compose.yaml \
  up -d --build
```

DNS-имя должно указывать на сервер, а TCP-порты `80` и `443` — доходить до него
через роутер. Caddy получит и будет обновлять TLS-сертификат автоматически.

## Telegram

При старте бот вызывает `setChatMenuButton`, поэтому рядом с полем ввода появится
кнопка **Тренироваться**. Для кнопки запуска (**Launch app**) в профиле бота дополнительно задай главное мини-приложение (**Main Mini App**)
в `@BotFather` на тот же URL.

Официальная схема проверки `initData` и настройки menu button:
[Telegram Mini Apps](https://core.telegram.org/bots/webapps).

## Локальная проверка

```sh
make
node --check docs/miniapp/app.js
```

Обычный браузер покажет экран «Открой приложение из Telegram»: это ожидаемо, потому что у него нет
подписанного Telegram `initData`. Полная сквозная проверка выполняется из кнопки меню в чате с ботом.

## Временный запуск в GitHub Codespaces

1. Добавь в GitHub Codespaces secret `TELEGRAM_BOT_TOKEN` с доступом к этому репозиторию.
2. Создай Codespace из актуальной ветки.
3. Убедись, что другая копия бота с тем же токеном остановлена.
4. Запусти:

```sh
./scripts/run_codespace.sh
```

Скрипт использует `CODESPACE_NAME` и `GITHUB_CODESPACES_PORT_FORWARDING_DOMAIN`,
которые GitHub добавляет в окружение. Если GitHub CLI не смог сделать порт публичным,
открой вкладку **PORTS**, нажми правой кнопкой на порт 8080 и выбери **Port Visibility → Public**.

## Безопасность

- сервер сравнивает HMAC в постоянное время и отклоняет `initData` старше заданного срока жизни (TTL);
- браузерная часть не получает `correct_option_id`, `correct_option_ids` или `accepted_answers`;
- сопоставление проверяется по одному выбору на сервере, связь пар в JSON не публикуется;
- не публикуй `.env`, bot token и `DATABASE_URL`;
- не выставляй HTTP-порт напрямую в интернет — только через обратный HTTPS-прокси.
