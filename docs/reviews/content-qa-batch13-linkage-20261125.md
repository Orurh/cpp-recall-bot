# CONTENT-QA 13 — linkage, ODR и единицы трансляции

8 рассмотрены, 7 изменены, 1 сохранена.

| Карточка | Решение | Механизм |
| --- | --- | --- |
| CR-000031 | KEEP | C language linkage и отличие от компиляции как C. |
| CR-000032 | REWORK | extern, declaration/definition, odr-use. |
| CR-000041 | REWORK | inline и условия нескольких определений. |
| CR-000042 | REWORK | inline thread_local, экземпляр на поток, transfer сохранён. |
| CR-000046 | REWORK | Четыре категории linkage и отличие от lookup. |
| CR-000056 | REWORK | ODR внутри и между TU, диагностика. |
| CR-000132 | REWORK | Namespace-scope static и отдельные сущности. |
| CR-000143 | REWORK | Preprocessing translation unit и translation unit. |

Код, ключи, IDs, уровни, семьи и transfer-разметка сохранены. Предыдущие SHA проверены; источники закреплены на N4950. Существующие справочные назначения и теория сохранены. Исторические fixture не изменены. Самопроверка; CI pending.
