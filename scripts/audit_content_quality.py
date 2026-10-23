#!/usr/bin/env python3
"""Heuristic QA report. This is NOT a semantic correctness validator."""
import argparse
import json
from collections import Counter
from pathlib import Path

parser = argparse.ArgumentParser()
parser.add_argument('--check', action='store_true', help='fail on strong answer-length cues')
args = parser.parse_args()

root = Path('content/questions')
questions = [json.loads(p.read_text(encoding='utf-8')) for p in sorted(root.rglob('*.json'))]
choices = [q for q in questions if q['task']['type'] == 'single_choice']
matching = [q for q in questions if q['task']['type'] == 'matching']
longest = []
strong_length_cues = []
positions = Counter()
for question in choices:
    task = question['task']
    lengths = [len(option['text']) for option in task['options']]
    index = next(i for i, o in enumerate(task['options']) if o['id'] == task['correct_option_id'])
    positions[index] += 1
    longest_incorrect = max(v for i, v in enumerate(lengths) if i != index)
    if lengths[index] > longest_incorrect:
        longest.append(question['id'])
    # A slightly longer answer is normal. Flag only a difference large enough
    # to reveal the key without reading the options semantically.
    if lengths[index] >= 40 and lengths[index] - longest_incorrect >= 20 \
            and lengths[index] > 1.45 * longest_incorrect:
        strong_length_cues.append((question['id'], lengths))

print(f'Вопросов: {len(questions)}; single_choice: {len(choices)}; matching: {len(matching)}')
print(f'Верный вариант строго длиннее всех остальных: {len(longest)}/{len(choices)}')
print('Позиции верного варианта в JSON:', dict(positions))
print('Mini App перемешивает варианты, поэтому позиция в JSON не видна учащемуся.')
print('Карточек с первичным источником:', sum(bool(q.get('source')) for q in questions))
if strong_length_cues:
    print('Сильная подсказка длиной правильного варианта:')
    for name, lengths in strong_length_cues:
        print(' ', name, lengths)
else:
    print('Сильных подсказок длиной правильного варианта не найдено.')
print('QA: эвристики; техническую истинность и правдоподобие дистракторов проверяйте редакторски.')

if args.check and strong_length_cues:
    raise SystemExit(1)
