# CONTENT-QA 18 — виртуальные функции и разрушение

13 рассмотрено, 12 изменено, 1 сохранена. ID, уровень, семья, ключи и transfer сохранены. Пять привязок заменены тремя направляющими статьями. Исторические статьи и реестры не изменены. Тесты исходных и изменённых примеров, диагностики компиляции и компоновки добавлены; полный CI ещё не выполнен. Самопроверка, не независимое ревью.

| ID | Карточка | Решение |
|---|---|---|
| CR-000057 | oop_abstract_class_definition | EDIT |
| CR-000062 | oop_covariant_virtual_return | EDIT |
| CR-000064 | oop_dynamic_cast_pointer_failure | EDIT |
| CR-000067 | oop_final_prevents_override | EDIT |
| CR-000071 | oop_nonvirtual_static_binding | EDIT |
| CR-000077 | oop_override_const_mismatch | EDIT |
| CR-000082 | oop_private_virtual_override | EDIT |
| CR-000085 | oop_pure_virtual_destructor_definition | EDIT |
| CR-000093 | oop_virtual_call_in_constructor | EDIT |
| CR-000095 | oop_virtual_destructor_reason | EDIT |
| CR-000097 | oop_virtual_dispatch_basic | EDIT |
| CR-000098 | oop_virtual_function_definition | EDIT |
| CR-000084 | oop_pure_virtual_definition | KEEP: точный синтаксис и объяснение final overrider |

Повторный аудит: восстановлены точные ограничения ковариантности; CR-000071 и CR-000097 теперь сопоставляют одинаковые вызовы с результатами AD и DD. Уточнён порядок вызовов деструкторов. Добавлены проверки допустимой и недопустимой cv-квалификации.
