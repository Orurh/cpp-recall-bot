"""Contact-aware pilot observations: server offers are not evidence of reading."""
from collections import Counter
from dataclasses import dataclass, field

from report_retrieval import MAX_EVENTS, identifier, known_metadata, require, timestamp

KINDS = {'question_delivered', 'feedback_delivered', 'reference_opened', 'theory_opened', 'answer_revealed'}
HELP = {'reference_opened', 'theory_opened', 'answer_revealed'}
OUTCOMES = ('changed_content', 'ambiguous_encounter', 'repair', 'assisted', 'unknown_assistance',
            'missing_delivery', 'insufficient_history', 'without_preparation', 'missing_prerequisites',
            'intervening_activity', 'previously_exposed', 'already_followed_up', 'early',
            'first_correct', 'first_incorrect', 'followup_correct', 'followup_incorrect')


def encounter_key(row):
    return (row['user_id'], row['session_id'], row.get('question_id'), row.get('revision'), row['attempt_number'])


def validate_document(document):
    require(isinstance(document, dict) and document.get('schema') == 'cpp-recall-study-input/v1')
    require(document.get('complete') is True and document.get('read_only') is True)
    until = timestamp(document.get('observed_until'))
    bounds = []
    for name in ('tracking_since', 'contacts_since'):
        bound = timestamp(document[name]) if document.get(name) is not None else None
        require(bound is None or bound <= until)
        bounds.append(bound)
    rows = document.get('events')
    require(isinstance(rows, list) and len(rows) <= MAX_EVENTS)
    events, ids, excluded = [], set(), 0
    for row in rows:
        require(isinstance(row, dict) and row.get('direction') in ('cpp', 'system_design'))
        if row['direction'] != 'cpp':
            excluded += 1
            continue
        require(identifier(row.get('id')) and row['id'] not in ids)
        ids.add(row['id'])
        require(type(row.get('user_id')) is int and row['user_id'] > 0)
        require(type(row.get('session_id')) is int and row['session_id'] > 0)
        require(identifier(row.get('question_id')))
        revision = row.get('revision')
        require(revision is None or isinstance(revision, str) and len(revision) <= 64)
        require(row.get('event') in ('answer', 'contact', 'help'))
        attempt = row.get('attempt_number')
        require((row['event'] == 'help' and attempt is None) or
                type(attempt) is int and 1 <= attempt <= 1000)
        if row['event'] == 'answer':
            require(type(row.get('correct')) is bool and type(row.get('help_known')) is bool)
            require(row.get('assisted') is None or type(row['assisted']) is bool)
        if row['event'] == 'contact':
            require(row.get('kind') in KINDS)
        at = timestamp(row.get('occurred_at'))
        require(at <= until)
        events.append((at, row['event'] == 'answer', row['id'], row))
    events.sort(key=lambda item: item[:3])
    return events, bounds, excluded


@dataclass
class Encounter:
    rows: list = field(default_factory=list)
    answers: list = field(default_factory=list)
    offered_at: object = None
    anchor: object = None
    activity: int = 0
    helped: bool = False
    processed: bool = False


def matches(row, card):
    return known_metadata(row) and all(row.get(name) == card[name] for name in ('question_id', 'revision', 'family_id'))


def classify(row, group, state, protocol, coverage, answer_simultaneous):
    if not matches(row, protocol['control']) or row['skill_id'] != protocol['skill_id'] or row['evidence_type'] != 'application':
        return 'changed_content'
    if len(group.answers) != 1:
        return 'ambiguous_encounter'
    if row['attempt_number'] > 1:
        return 'repair'
    if row['assisted'] is True or group.helped:
        return 'assisted'
    if not row['help_known'] or row['assisted'] is None:
        return 'unknown_assistance'
    anchor = group.anchor
    if anchor is None:
        return 'missing_delivery'
    last = anchor['last']
    if (coverage is None or group.offered_at < coverage or anchor['simultaneous'] or answer_simultaneous or
            group.offered_at >= timestamp(row['occurred_at']) or
            state['unknown_count'] != anchor['unknown_count'] or
            (last is not None and (last < coverage or anchor['unknown_at'] is not None and anchor['unknown_at'] >= last))):
        return 'insufficient_history'
    if not anchor['prepared'] or last is None:
        return 'without_preparation'
    if not set(protocol['required_skills']).issubset(anchor['prerequisites']):
        return 'missing_prerequisites'
    if state['activity'] - anchor['activity'] != group.activity - anchor['own_activity']:
        return 'intervening_activity'
    if anchor['seen'] and not anchor['initial']:
        return 'previously_exposed'
    if state['followed_up']:
        return 'already_followed_up'
    hours = protocol['followup_pause_hours'] if anchor['seen'] else protocol['pause_hours']
    if (group.offered_at - last).total_seconds() < hours * 3600:
        return 'early'
    return ('followup_' if anchor['seen'] else 'first_') + ('correct' if row['correct'] else 'incorrect')


def build_contact_pilot_report(document, protocol):
    events, (tracking, contacts), excluded = validate_document(document)
    coverage = max(tracking, contacts) if tracking is not None and contacts is not None else None
    groups = {}
    related = {protocol['skill_id'], *protocol['required_skills']}
    simultaneous, own_simultaneous, prerequisite_events = Counter(), Counter(), Counter()
    for at, _, _, row in events:
        known = known_metadata(row)
        key = encounter_key(row)
        if row['event'] != 'help':
            group = groups.setdefault(key, Encounter())
            group.rows.append(row)
            if row['event'] == 'answer':
                group.answers.append((at, row))
            elif row['kind'] == 'question_delivered' and group.offered_at is None:
                group.offered_at = at
        if not known or row['skill_id'] in related:
            simultaneous[(row['user_id'], at)] += 1
            own_simultaneous[(key, at)] += 1
        if known and (row['event'] in ('answer', 'help') or row.get('kind') in HELP):
            prerequisite_events[(row['user_id'], row['skill_id'], at)] += 1
    # One archive identity must not carry conflicting known metadata in an input.
    for group in groups.values():
        signatures = {(row['skill_id'], row['family_id'], row['evidence_type'])
                      for row in group.rows if known_metadata(row)}
        require(len(signatures) <= 1)
    control_groups = [group for key, group in groups.items() if key[2] == protocol['control']['question_id']]
    report = dict(schema='cpp-recall-transfer-pilot-report/v2', protocol_id=protocol['id'],
                  observed_until=document['observed_until'], tracking_since=document.get('tracking_since'),
                  contacts_since=document.get('contacts_since'), pause_hours=protocol['pause_hours'],
                  followup_pause_hours=protocol['followup_pause_hours'], control_encounters=len(control_groups),
                  control_answers=sum(len(group.answers) for group in control_groups),
                  unanswered_encounters=sum(not group.answers for group in control_groups),
                  outcomes=dict.fromkeys(OUTCOMES, 0), first_recorded=dict(total=0, correct=0, incorrect=0),
                  followup=dict(total=0, correct=0, incorrect=0), excluded_other_directions=excluded,
                  system_design='not_assessed', interpretation='recorded_context_candidates_not_mastery',
                  unrecorded_exposure='unknown', observation_basis='server_offers_not_reading')
    states, legacy_help = {}, set()
    for at, is_answer, _, row in events:
        state = states.setdefault(row['user_id'], dict(last=None, unknown_at=None, unknown_count=0,
            activity=0, prepared=False, prerequisites=set(), first_control=None, initial=False, followed_up=False))
        known = known_metadata(row)
        key = encounter_key(row)
        group = groups.get(key) if row['event'] != 'help' else None
        if row['event'] == 'help':
            legacy_help.add(key[:-1])
        if is_answer and key[:-1] in legacy_help:
            group.helped = True
        control = row['question_id'] == protocol['control']['question_id']
        target = known and row['skill_id'] == protocol['skill_id']
        if (group is not None and row['event'] == 'contact' and row['kind'] == 'question_delivered' and
                group.anchor is None):
            group.anchor = dict(last=state['last'], unknown_at=state['unknown_at'], unknown_count=state['unknown_count'],
                activity=state['activity'], own_activity=group.activity, prepared=state['prepared'],
                prerequisites=set(state['prerequisites']), initial=state['initial'],
                seen=state['first_control'] is not None and state['first_control'] != key,
                simultaneous=simultaneous[(row['user_id'], at)] > own_simultaneous[(key, at)])
        if control and is_answer and not group.processed:
            # A same-time answer from another encounter can sort after this one;
            # do not infer chronology from IDs at either end of the interval.
            outcome = classify(row, group, state, protocol, coverage,
                simultaneous[(row['user_id'], at)] > own_simultaneous[(key, at)])
            report['outcomes'][outcome] += 1
            group.processed = True
            if outcome.startswith(('first_', 'followup_')):
                familiar = outcome.startswith('followup_')
                bucket = report['followup' if familiar else 'first_recorded']
                bucket['total'] += 1
                bucket['correct' if row['correct'] else 'incorrect'] += 1
                state['followed_up' if familiar else 'initial'] = True
        if control and state['first_control'] is None:
            state['first_control'] = key if group is not None else ('legacy', row['id'])
        if group is not None and (row['event'] == 'contact' and row['kind'] in HELP):
            # Only opens before an answer can affect its independence. Later
            # disclosures still refresh the skill activity used by future trials.
            group.helped = True
        if not known:
            state['unknown_at'] = at
            state['unknown_count'] += 1
            continue
        tracked = tracking is not None and at >= tracking
        if row['skill_id'] in protocol['required_skills'] and tracked:
            if is_answer:
                clean = (row['attempt_number'] == 1 and row['correct'] and row['assisted'] is False and
                         row['help_known'] and prerequisite_events[(row['user_id'], row['skill_id'], at)] == 1 and
                         not group.helped and len(group.answers) == 1)
                if clean:
                    state['prerequisites'].add(row['skill_id'])
                else:
                    state['prerequisites'].discard(row['skill_id'])
            elif row['event'] == 'help' or row.get('kind') in ('reference_opened', 'theory_opened'):
                state['prerequisites'].discard(row['skill_id'])
            elif row.get('kind') == 'answer_revealed' and not (group.answers and group.answers[0][0] <= at and
                    group.answers[0][1]['correct'] and group.answers[0][1]['assisted'] is False):
                state['prerequisites'].discard(row['skill_id'])
        if target:
            if tracked and is_answer and row['correct'] and row['help_known'] and matches(row, protocol['preparation']):
                state['prepared'] = True
            state['last'] = at
            state['activity'] += 1
            if group is not None:
                group.activity += 1
    return report
