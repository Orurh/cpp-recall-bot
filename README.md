## C++ Recall Bot

Telegram-тренажёр по C++ с возвратом ошибочных вопросов в очередь и историей попыток в SQLite.

### Запуск

```bash
cp .env.example .env
# заполнить TELEGRAM_BOT_TOKEN, затем экспортировать переменные из .env
make run
```

`DATABASE_PATH` необязателен; по умолчанию используется `cpp-recall-bot.sqlite3` в текущем каталоге. Схема создаётся автоматически при старте.
