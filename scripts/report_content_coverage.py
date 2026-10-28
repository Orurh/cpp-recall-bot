#!/usr/bin/env python3
"""Read-only structural coverage, not a technical or pedagogical acceptance."""
from __future__ import annotations

import argparse
from collections import Counter
import hashlib
import json
from pathlib import Path
import subprocess
import sys


ROOT = Path(__file__).resolve().parent.parent
GAPS = {
    'no_cards': 'нет карточек',
    'few_families': 'меньше двух семейств',
    'no_application': 'нет прикладного семейства',
    'no_transfer': 'нет помеченного переноса',
    'no_reference': 'нет справки до ответа',
    'no_route_practice': 'нет практики в пути',
    'no_route_independent': 'нет самостоятельного шага пути',
}
LIMITATIONS = [
    'Это структура банка, не оценка знаний пользователя и не приёмка беты.',
    'Семейства и перенос — редакционная разметка; их смысловая независимость здесь не проверяется.',
    'Практика и самостоятельный шаг определяются только явными этапами пути, не уровнем или словами условия.',
    'Связанная справка и разбор после ошибки не доказывают наличие достаточного введения.',
    'Принятая редакция означает совпадение записи рецензии, не истинность ответа или независимость проверки.',
    'Сложность — текущая редакционная метка, не измеренная трудность.',
]
REVIEW_LABELS = {'accepted_revision': 'принятая редакция', 'missing': 'нет записи',
                 'stale': 'редакция не совпадает', 'not_accepted': 'не принята'}


def unique_index(items: list[dict], name: str) -> dict[str, dict]:
    result = {}
    for item in items:
        key = item['id']
        if not key or key in result:
            raise ValueError(f'duplicate or empty {name}: {key}')
        result[key] = item
    return result


def load_reviews(directory: Path) -> dict[str, dict]:
    if not directory.is_dir():
        raise ValueError('review directory is unavailable')
    result = {}
    for path in sorted(directory.glob('*.json')):
        document = json.loads(path.read_text(encoding='utf-8'))
        if document['schema_version'] != 1:
            raise ValueError(f'unsupported review schema: {path.name}')
        reviewer = document['reviewer']
        independent = reviewer.get('independent') is True and reviewer.get('mode') == 'independent_review'
        for record in document['reviews']:
            qid = record['question_id']
            if qid in result:
                raise ValueError(f'duplicate review: {qid}')
            result[qid] = {**record, 'independent': independent}
    return result


def review_state(card: dict, reviews: dict[str, dict]) -> dict:
    record = reviews.get(card['id'])
    if record is None:
        return {'status': 'missing', 'independent': False}
    matched = all(record.get(left) == card[right] for left, right in (
        ('content_revision', 'content_revision'), ('public_id', 'public_id'),
        ('skill_id', 'skill_id'), ('question_family_id', 'family_id'), ('standard', 'standard')))
    status = 'stale' if not matched else 'accepted_revision' if record.get('decision') == 'accepted' else 'not_accepted'
    return {'status': status, 'independent': status == 'accepted_revision' and record['independent']}


def summarize(cards: list[dict]) -> dict:
    return {
        'cards': len(cards),
        'families': len({q['family_id'] for q in cards}),
        'application_families': len({q['family_id'] for q in cards if q['evidence_type'] == 'application'}),
        'tagged_transfer_families': len({q['family_id'] for q in cards if q['transfer']}),
        'reference_cards': sum(bool(q['references']) for q in cards),
        'route_practice_cards': sum(any(s['kind'] == 'practice' for s in q['route_stages']) for q in cards),
        'route_independent_cards': sum(any(s['kind'] == 'independent' for s in q['route_stages']) for q in cards),
        'accepted_revisions': sum(q['review']['status'] == 'accepted_revision' for q in cards),
        'independent_reviews': sum(q['review']['independent'] for q in cards),
        'review_states': dict(sorted(Counter(q['review']['status'] for q in cards).items())),
    }


def build_report(catalog: dict, reviews: dict[str, dict], topic: str | None = None,
                 skill: str | None = None) -> dict:
    if catalog['schema_version'] != 1:
        raise ValueError('unsupported coverage catalog schema')
    topics = unique_index(catalog['topics'], 'topic')
    skills = unique_index(catalog['skills'], 'skill')
    references = unique_index(catalog['references'], 'reference')
    cards = unique_index(catalog['questions'], 'question')
    if set(reviews) - set(cards):
        raise ValueError('review refers to an unknown question')
    unique_index(catalog['routes'], 'route')
    if topic is not None and topic not in topics:
        raise ValueError(f'unknown topic: {topic}')
    if skill is not None and (skill not in skills or (topic and skills[skill]['topic'] != topic)):
        raise ValueError(f'unknown skill or incompatible topic: {skill}')
    for definition in skills.values():
        if definition['topic'] not in topics or any(p not in skills for p in definition['prerequisites']):
            raise ValueError(f'invalid skill catalog: {definition["id"]}')
    for card in cards.values():
        if card['skill_id'] not in skills or card['topic'] != skills[card['skill_id']]['topic']:
            raise ValueError(f'unknown skill or topic mismatch: {card["id"]}')
        if type(card['level']) is not int or not 1 <= card['level'] <= 10:
            raise ValueError(f'invalid level: {card["id"]}')
        if not card['family_id'] or card['evidence_type'] not in ('recognition', 'application'):
            raise ValueError(f'invalid evidence: {card["id"]}')
        if type(card['transfer']) is not bool or (card['transfer'] and card['evidence_type'] != 'application'):
            raise ValueError(f'invalid transfer: {card["id"]}')
        if len(card['references']) != len(set(card['references'])) or any(r not in references for r in card['references']):
            raise ValueError(f'invalid reference binding: {card["id"]}')
    stages = {qid: [] for qid in cards}
    for route in catalog['routes']:
        required = route.get('requires_retention', True)
        if type(required) is not bool or (required and route['retention_skill_id'] not in skills) or (
                not required and route['retention_skill_id'] != ''):
            raise ValueError(f'invalid route target: {route["id"]}')
        unique_index(route['stages'], 'stage')
        for stage in route['stages']:
            if stage['kind'] not in ('practice', 'independent'):
                raise ValueError(f'unknown stage kind: {stage["kind"]}')
            if not required and stage['kind'] != 'practice':
                raise ValueError('practice-only route cannot certify an independent stage')
            for qid in stage['question_ids']:
                if qid not in cards:
                    raise ValueError(f'unknown route card: {qid}')
                stages[qid].append({'route_id': route['id'], 'stage_id': stage['id'], 'kind': stage['kind']})
    theory = {sid: [] for sid in skills}
    unique_index(catalog['theory'], 'theory block')
    for block in catalog['theory']:
        if block['skill_id'] not in skills:
            raise ValueError(f'unknown theory skill: {block["id"]}')
        theory[block['skill_id']].append(block['id'])
    enriched = [{**q, 'route_stages': sorted(stages[q['id']], key=lambda s: (s['route_id'], s['stage_id'])),
                 'review': review_state(q, reviews)} for q in sorted(cards.values(), key=lambda q: q['public_id'])]
    selected = [q for q in enriched if (topic is None or q['topic'] == topic) and
                (skill is None or q['skill_id'] == skill)]
    rows = []
    for sid, definition in sorted(skills.items()):
        if (topic and definition['topic'] != topic) or (skill and sid != skill):
            continue
        group = [q for q in selected if q['skill_id'] == sid]
        counts = summarize(group)
        gaps = ['no_cards'] if not group else [name for name, missing in (
            ('few_families', counts['families'] < 2), ('no_application', counts['application_families'] == 0),
            ('no_transfer', counts['tagged_transfer_families'] == 0), ('no_reference', counts['reference_cards'] == 0),
            ('no_route_practice', counts['route_practice_cards'] == 0),
            ('no_route_independent', counts['route_independent_cards'] == 0)) if missing]
        rows.append({**definition, **counts, 'theory_block_ids': sorted(theory[sid]), 'gaps': gaps,
                     'introduction_status': 'not_assessed',
                     'levels': [{'level': level, **summarize([q for q in group if q['level'] == level])}
                                for level in range(1, 11)]})
    topic_rows = [{**definition, **summarize([q for q in selected if q['topic'] == tid])}
                  for tid, definition in sorted(topics.items())
                  if (topic is None or tid == topic) and (skill is None or tid == skills[skill]['topic'])]
    digest = hashlib.sha256(json.dumps(catalog, sort_keys=True, ensure_ascii=False,
                                      separators=(',', ':')).encode()).hexdigest()
    return {'schema_version': 1, 'catalog_metadata_sha256': digest,
            'scope': {'topic': topic, 'skill': skill}, 'limitations': LIMITATIONS,
            'catalog_totals': {'topics': len(topics), 'skills': len(skills),
                               'assessable_skills': sum(s['assessable'] for s in skills.values()),
                               'reference_articles': len(references), 'theory_blocks': len(catalog['theory']),
                               'routes': len(catalog['routes'])},
            'bank_totals': summarize(enriched), 'totals': summarize(selected),
            'topics': topic_rows, 'skills': rows, 'questions': selected}


def cell(value: object) -> str:
    return str(value).replace('|', '\\|').replace('\n', ' ')


def markdown(report: dict) -> str:
    totals = report['totals']
    catalog = report['catalog_totals']
    lines = ['# Структурное покрытие банка', '', *[f'- {line}' for line in report['limitations']], '',
             f'В выборке — карточек: {totals["cards"]}; семейств: {totals["families"]}. '
             f'Справка: {totals["reference_cards"]}/{totals["cards"]}. '
             f'Принятые редакции: {totals["accepted_revisions"]}; независимые рецензии: {totals["independent_reviews"]}.',
             f'Всего карточек в банке: {report["bank_totals"]["cards"]}. '
             f'Навыков в каталоге: {catalog["skills"]}, из них в оценке карты: {catalog["assessable_skills"]}.',
             'Состояния записей рецензий: ' + (', '.join(f'{REVIEW_LABELS[name]}: {count}' for name, count in totals['review_states'].items()) or 'нет карточек в выборке') + '.',
             f'Отпечаток метаданных каталога: `{report["catalog_metadata_sha256"]}`.', '',
             '## Темы', '', '| Тема | Карточки | Семейства | Прикладные семейства | Справка до ответа |',
             '| --- | ---: | ---: | ---: | ---: |']
    for row in report['topics']:
        lines.append(f'| {cell(row["title"])} (`{row["id"]}`) | {row["cards"]} | {row["families"]} | '
                     f'{row["application_families"]} | {row["reference_cards"]}/{row["cards"]} |')
    lines += ['', '## Навыки и пробелы', '',
              '«Путь» ниже — число карточек в явно обозначенных практических/самостоятельных этапах. '
              'Наличие достаточного введения не оценено. '
              'Отсутствие этапа не означает отсутствие знаний у пользователя.', '',
              '| Навык | Карточки / семейства | Сложность: карточки | Справка | Путь: практика / самост. | Сигналы для редактора |',
              '| --- | ---: | --- | ---: | ---: | --- |']
    for row in report['skills']:
        levels = ', '.join(f'L{r["level"]}: {r["cards"]}' for r in row['levels'] if r['cards']) or '—'
        gaps = '; '.join(GAPS[g] for g in row['gaps']) or 'перечисленных структурных пробелов нет; качество не оценено'
        lines.append(f'| {cell(row["title"])} (`{row["id"]}`) | {row["cards"]} / {row["families"]} | {levels} | '
                     f'{row["reference_cards"]}/{row["cards"]} | {row["route_practice_cards"]} / '
                     f'{row["route_independent_cards"]} | {gaps} |')
    if report['scope']['skill']:
        row = report['skills'][0]
        lines += ['', '## Детали выбранного навыка', '',
                  'Предпосылки: ' + (', '.join(f'`{p}`' for p in row['prerequisites']) or 'не заданы') + '.',
                  'Разборы после ошибки: ' + (', '.join(row['theory_block_ids']) or 'нет') + '.',
                  'Наличие достаточного введения: не оценено.',
                  f'В оценке карты: {"да" if row["assessable"] else "нет"}.', '',
                  '| Сложность | Карточки | Семейства | Прикладные | Перенос (разметка) |',
                  '| --- | ---: | ---: | ---: | ---: |']
        for level in row['levels']:
            lines.append(f'| L{level["level"]} | {level["cards"]} | {level["families"]} | '
                         f'{level["application_families"]} | {level["tagged_transfer_families"]} |')
        lines += ['', '| Карточка | L | Семейство | Форма | Справка | Рецензия редакции |',
                  '| --- | ---: | --- | --- | --- | --- |']
        for q in report['questions']:
            form = ('прикладная' if q['evidence_type'] == 'application' else 'распознавание')
            form += ', перенос (разметка)' if q['transfer'] else ''
            lines.append(f'| {q["public_id"]} (`{q["id"]}`) | {q["level"]} | `{q["family_id"]}` | {form} | '
                         f'{cell(", ".join(q["references"]) or "нет")} | {REVIEW_LABELS[q["review"]["status"]]} |')
    return '\n'.join(lines) + '\n'


def main() -> int:
    parser = argparse.ArgumentParser(description='Структура банка без изменения приложения и данных.')
    parser.add_argument('--content-check', type=Path, default=ROOT / 'build/content_check')
    parser.add_argument('--format', choices=('markdown', 'json'), default='markdown')
    parser.add_argument('--topic', help='ID темы, например move_semantics')
    parser.add_argument('--skill', help='ID навыка, например references.fundamentals')
    args = parser.parse_args()
    try:
        process = subprocess.run([str(args.content_check.resolve()), '--coverage-json', str(ROOT / 'content/questions')],
                                 cwd=ROOT, check=True, capture_output=True, text=True, timeout=30)
        report = build_report(json.loads(process.stdout), load_reviews(ROOT / 'docs/reviews/content'), args.topic, args.skill)
        print(json.dumps(report, ensure_ascii=False, indent=2) if args.format == 'json' else markdown(report), end='\n' if args.format == 'json' else '')
        return 0
    except (OSError, ValueError, KeyError, TypeError, subprocess.SubprocessError) as error:
        print(f'Отчёт не построен: {error}', file=sys.stderr)
        return 2


if __name__ == '__main__':
    raise SystemExit(main())
