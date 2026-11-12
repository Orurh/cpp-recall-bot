#!/usr/bin/env python3
"""Aggregate recorded retrieval observations offline. Never connect to a database."""
import argparse
from collections import Counter
from datetime import datetime, timezone
import json
import re
import sys

MAX_BYTES = 16 * 1024 * 1024
MAX_EVENTS = 50000
OUTCOMES = ('first_observation', 'repair', 'assisted', 'unknown_assistance',
            'insufficient_metadata', 'insufficient_history', 'short_pause',
            'delayed_correct', 'delayed_incorrect')
FAMILIARITY = ('familiar_card', 'changed_revision', 'family_variant', 'other_family_unverified_context')


def require(condition, message='Некорректные данные отчёта.'):
    if not condition:
        raise ValueError(message)


def timestamp(value):
    require(isinstance(value, str) and len(value) <= 40)
    try:
        result = datetime.fromisoformat(value.replace('Z', '+00:00'))
    except ValueError:
        raise ValueError('Некорректная временная отметка.') from None
    require(result.tzinfo is not None, 'Нужен часовой пояс временной отметки.')
    return result.astimezone(timezone.utc)


def identifier(value):
    return isinstance(value, str) and 0 < len(value) <= 256


def known_metadata(row):
    return (all(identifier(row.get(field)) for field in ('question_id', 'skill_id', 'family_id'))
            and isinstance(row.get('revision'), str)
            and re.fullmatch(r'[0-9a-f]{64}', row['revision']) is not None
            and row.get('evidence_type') in ('recognition', 'application'))


def build_report(document, minimum_pause_hours=24):
    require(type(minimum_pause_hours) is int and 1 <= minimum_pause_hours <= 720)
    require(isinstance(document, dict) and document.get('schema') == 'cpp-recall-retrieval-input/v1')
    require(document.get('complete') is True, 'Неполный экспорт: расчёт отклонён.')
    until = timestamp(document.get('observed_until'))
    tracking = timestamp(document['tracking_since']) if document.get('tracking_since') is not None else None
    require(tracking is None or tracking <= until)
    raw_events = document.get('events')
    require(isinstance(raw_events, list) and len(raw_events) <= MAX_EVENTS)
    events, ids, excluded = [], set(), 0
    for row in raw_events:
        require(isinstance(row, dict) and row.get('direction') in ('cpp', 'system_design'))
        if row['direction'] != 'cpp':
            excluded += 1
            continue
        key = row.get('id')
        require(identifier(key) and key not in ids)
        ids.add(key)
        require(type(row.get('user_id')) is int and row['user_id'] > 0)
        require(row.get('event') in ('answer', 'help'))
        at = timestamp(row.get('occurred_at'))
        require(at <= until, 'Событие выходит за границу наблюдения.')
        if row['event'] == 'answer':
            require(type(row.get('correct')) is bool)
            require(type(row.get('attempt_number')) is int and 1 <= row['attempt_number'] <= 1000)
            require(row.get('assisted') is None or type(row['assisted']) is bool)
            require(type(row.get('help_known')) is bool)
        events.append((at, known_metadata(row), row['event'] == 'answer', key, row))
    # Unknown same-time activity must invalidate history before any known answer,
    # regardless of event IDs. Known help also precedes same-time answers.
    events.sort(key=lambda entry: entry[:4])
    simultaneous_answers = Counter((row['user_id'], row['skill_id'], at)
        for at, metadata, is_answer, _, row in events if metadata and is_answer)
    counts = dict.fromkeys(OUTCOMES, 0)
    delayed = dict(total=0, correct=0, incorrect=0, correct_percent=None,
                   familiarity=dict.fromkeys(FAMILIARITY, 0),
                   evidence_type=dict(recognition=0, application=0))
    report = dict(schema='cpp-recall-retrieval-report/v1', minimum_pause_hours=minimum_pause_hours,
                  observed_until=until.isoformat(), tracking_since=tracking.isoformat() if tracking else None,
                  answers=0, help_events=0, unknown_metadata_events=0,
                  excluded_other_directions=excluded, outcomes=counts, delayed=delayed,
                  system_design='not_assessed', interpretation='recorded_observations_not_mastery')
    previous, unmapped, seen_cards, seen_questions, seen_families = {}, {}, set(), set(), set()
    for at, metadata_known, _, _, row in events:
        user = row['user_id']
        is_answer = row['event'] == 'answer'
        report['answers' if is_answer else 'help_events'] += 1
        if not metadata_known:
            report['unknown_metadata_events'] += 1
            unmapped[user] = at
            if is_answer:
                counts['insufficient_metadata'] += 1
            continue
        key = (user, row['skill_id'])
        card = (*key, row['question_id'], row['revision'])
        question = (*key, row['question_id'])
        family = (*key, row['family_id'])
        familiarity = ('familiar_card' if card in seen_cards else 'changed_revision'
                       if question in seen_questions else 'family_variant'
                       if family in seen_families else 'other_family_unverified_context')
        prior = previous.get(key)
        if is_answer:
            if row['attempt_number'] > 1:
                outcome = 'repair'
            elif row.get('assisted') is True:
                outcome = 'assisted'
            elif (not row['help_known'] or row.get('assisted') is None
                  or tracking is None or at < tracking):
                outcome = 'unknown_assistance'
            elif ((user in unmapped and (prior is None or unmapped[user] >= prior))
                  or (prior is not None and prior < tracking)
                  or simultaneous_answers[(*key, at)] > 1):
                outcome = 'insufficient_history'
            elif prior is None:
                outcome = 'first_observation'
            elif (at - prior).total_seconds() < minimum_pause_hours * 3600:
                outcome = 'short_pause'
            else:
                outcome = 'delayed_correct' if row['correct'] else 'delayed_incorrect'
                delayed['total'] += 1
                delayed['correct' if row['correct'] else 'incorrect'] += 1
                delayed['familiarity'][familiarity] += 1
                delayed['evidence_type'][row['evidence_type']] += 1
            counts[outcome] += 1
        # A wrong answer or help without an answer is still relevant study activity.
        previous[key] = at
        seen_cards.add(card)
        seen_questions.add(question)
        seen_families.add(family)
    if delayed['total']:
        delayed['correct_percent'] = round(100 * delayed['correct'] / delayed['total'], 1)
    return report


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--pause-hours', type=int, default=24,
                        help='Minimum observed pause, 1–720 hours; not a memory-model optimum.')
    args = parser.parse_args()
    try:
        raw = sys.stdin.buffer.read(MAX_BYTES + 1)
        require(len(raw) <= MAX_BYTES, 'Экспорт превышает 16 MiB.')
        document = json.loads(raw)
        report = build_report(document, args.pause_hours)
    except (ValueError, RecursionError, OverflowError):
        print('Некорректные данные или неполный экспорт: отчёт не рассчитан.', file=sys.stderr)
        return 2
    print(json.dumps(report, ensure_ascii=False, indent=2))
    return 0


if __name__ == '__main__':
    sys.exit(main())
