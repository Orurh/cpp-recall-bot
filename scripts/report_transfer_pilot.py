#!/usr/bin/env python3
"""Classify a reviewed context pilot offline; never award mastery or change learning."""
import argparse
from collections import Counter
import json
from pathlib import Path
import re
import sys

from report_retrieval import MAX_BYTES, build_report, identifier, known_metadata, require, timestamp

DEFAULT_PROTOCOL = Path(__file__).resolve().parents[1] / 'content/transfer_pilots/raii_state_restore.json'
OUTCOMES = ('changed_content', 'repair', 'assisted', 'unknown_assistance', 'insufficient_history',
            'without_preparation', 'missing_prerequisites', 'previously_exposed', 'early',
            'first_correct', 'first_incorrect', 'followup_correct', 'followup_incorrect', 'already_followed_up')


def validate_protocol(protocol):
    require(isinstance(protocol, dict) and protocol.get('schema') == 'cpp-recall-transfer-pilot/v1')
    require(identifier(protocol.get('id')) and identifier(protocol.get('skill_id')))
    pause, followup = protocol.get('pause_hours'), protocol.get('followup_pause_hours')
    require(type(pause) is int and type(followup) is int and 1 <= pause < followup <= 720)
    skills = protocol.get('required_skills')
    require(isinstance(skills, list) and 1 <= len(skills) <= 8 and all(identifier(skill) for skill in skills))
    require(len(set(skills)) == len(skills) and protocol['skill_id'] not in skills)
    for field in ('preparation', 'control'):
        card = protocol.get(field)
        require(isinstance(card, dict) and identifier(card.get('question_id')) and identifier(card.get('family_id')))
        require(isinstance(card.get('revision'), str) and re.fullmatch(r'[0-9a-f]{64}', card['revision']) is not None)
    require(protocol['preparation']['question_id'] != protocol['control']['question_id'])


def matches(row, card):
    return all(row.get(key) == card[key] for key in ('question_id', 'revision', 'family_id'))


def build_pilot_report(document, protocol):
    validate_protocol(protocol)
    # Reuse the complete bounded v1 input validation, not a more permissive adapter.
    build_report(document, protocol['pause_hours'])
    tracking = timestamp(document['tracking_since']) if document.get('tracking_since') is not None else None
    events = [(timestamp(row['occurred_at']), known_metadata(row), row['event'] == 'answer', row['id'], row)
              for row in document['events'] if row['direction'] == 'cpp']
    events.sort(key=lambda entry: entry[:4])
    related_skills = {protocol['skill_id'], *protocol['required_skills']}
    # Equal-time prerequisite work cannot be assumed to precede the control.
    simultaneous = Counter((row['user_id'], at) for at, known, _, _, row in events
                           if known and row['skill_id'] in related_skills)
    same_skill_events = Counter((row['user_id'], row['skill_id'], at)
                                for at, known, _, _, row in events if known)
    report = dict(schema='cpp-recall-transfer-pilot-report/v1', protocol_id=protocol['id'],
                  observed_until=document['observed_until'], tracking_since=document.get('tracking_since'),
                  pause_hours=protocol['pause_hours'], followup_pause_hours=protocol['followup_pause_hours'],
                  control_answers=0, outcomes=dict.fromkeys(OUTCOMES, 0),
                  first_recorded=dict(total=0, correct=0, incorrect=0),
                  followup=dict(total=0, correct=0, incorrect=0), system_design='not_assessed',
                  interpretation='recorded_context_candidates_not_mastery',
                  unrecorded_exposure='unknown')
    states = {}
    for at, known, is_answer, _, row in events:
        state = states.setdefault(row['user_id'], dict(last=None, unknown=None, prepared=False,
                                  prerequisites=set(), seen=False, initial=False, followed_up=False))
        control = row.get('question_id') == protocol['control']['question_id']
        target = known and row['skill_id'] == protocol['skill_id']
        tracked = tracking is not None and at >= tracking
        reviewed = (known and target and matches(row, protocol['control'])
                    and row['evidence_type'] == 'application')
        if control and is_answer:
            report['control_answers'] += 1
            last = state['last']
            if not reviewed:
                outcome = 'changed_content'
            elif row['attempt_number'] > 1:
                outcome = 'repair'
            elif row['assisted'] is True:
                outcome = 'assisted'
            elif not tracked or not row['help_known'] or row['assisted'] is None:
                outcome = 'unknown_assistance'
            elif ((last is not None and last < tracking)
                  or (state['unknown'] is not None and (last is None or state['unknown'] >= last))
                  or simultaneous[(row['user_id'], at)] > 1):
                outcome = 'insufficient_history'
            elif not state['prepared'] or last is None:
                outcome = 'without_preparation'
            elif not set(protocol['required_skills']).issubset(state['prerequisites']):
                outcome = 'missing_prerequisites'
            elif state['seen'] and not state['initial']:
                outcome = 'previously_exposed'
            elif state['followed_up']:
                outcome = 'already_followed_up'
            elif (at - last).total_seconds() < (protocol['followup_pause_hours'] if state['seen']
                                                else protocol['pause_hours']) * 3600:
                outcome = 'early'
            else:
                bucket = 'followup' if state['seen'] else 'first_recorded'
                outcome = ('followup_' if state['seen'] else 'first_') + ('correct' if row['correct'] else 'incorrect')
                report[bucket]['total'] += 1
                report[bucket]['correct' if row['correct'] else 'incorrect'] += 1
                state['followed_up' if state['seen'] else 'initial'] = True
            report['outcomes'][outcome] += 1
        if control:
            # Help, an early answer or a changed revision consumes first recorded exposure too.
            state['seen'] = True
        if not known or (control and not reviewed):
            state['unknown'] = at
            continue
        if row['skill_id'] in protocol['required_skills'] and tracked:
            if (is_answer and row['attempt_number'] == 1 and row['correct']
                    and row['assisted'] is False and row['help_known']
                    and same_skill_events[(row['user_id'], row['skill_id'], at)] == 1):
                state['prerequisites'].add(row['skill_id'])
            else:
                state['prerequisites'].discard(row['skill_id'])
        if target:
            if (tracked and is_answer and row['correct'] and row['help_known']
                    and matches(row, protocol['preparation'])):
                # Completion can be assisted: it is preparation, not independent evidence.
                state['prepared'] = True
            state['last'] = at
    return report


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--protocol', type=Path, default=DEFAULT_PROTOCOL,
                        help='Reviewed local protocol JSON; no connection settings.')
    args = parser.parse_args()
    try:
        with args.protocol.open('rb') as source:
            config = source.read(16385)
        require(len(config) <= 16384)
        protocol = json.loads(config)
        raw = sys.stdin.buffer.read(MAX_BYTES + 1)
        require(len(raw) <= MAX_BYTES)
        report = build_pilot_report(json.loads(raw), protocol)
    except (ValueError, OSError, RecursionError, OverflowError):
        print('Некорректный протокол или неполная история: отчёт не рассчитан.', file=sys.stderr)
        return 2
    print(json.dumps(report, ensure_ascii=False, indent=2))
    return 0


if __name__ == '__main__':
    sys.exit(main())
