#!/usr/bin/env python3
"""Heuristic QA report. This is NOT a semantic correctness validator."""
import json
from collections import Counter
from pathlib import Path

root = Path('content/questions')
questions = [json.loads(p.read_text(encoding='utf-8')) for p in sorted(root.rglob('*.json'))]
choices = [q for q in questions if q['task']['type'] == 'single_choice']
matching = [q for q in questions if q['task']['type'] == 'matching']
longest = []
imbalanced = []
positions = Counter()
for question in choices:
    task = question['task']
    lengths = [len(option['text']) for option in task['options']]
    index = next(i for i, o in enumerate(task['options']) if o['id'] == task['correct_option_id'])
    positions[index] += 1
    if lengths[index] > max(v for i, v in enumerate(lengths) if i != index):
        longest.append(question['id'])
    # Numeric-only and short output questions have deliberately unequal labels.
    if max(lengths) > 40 and max(lengths) > 2 * max(1, min(lengths)):
        imbalanced.append((question['id'], lengths))

print(f'Вопросов: {len(questions)}; single_choice: {len(choices)}; matching: {len(matching)}')
print(f'Верный вариант строго длиннее всех остальных: {len(longest)}/{len(choices)}')
print('Позиции верного варианта в JSON:', dict(positions))
print('Примечание: если Telegram перемешивает варианты, позиции в JSON не видны учащемуся.')
if longest:
    print('Проверить вручную (длинный правильный):', ', '.join(longest))
if imbalanced:
    print('Проверить баланс длины (эвристика):')
    for name, lengths in imbalanced:
        print(' ', name, lengths)
print('QA: эвристики; техническую истинность и правдоподобие дистракторов проверяйте редакторски.')
