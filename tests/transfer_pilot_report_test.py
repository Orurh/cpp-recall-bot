"""MEM-03A: real offline CLI on synthetic histories, no database or network."""
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
SCRIPT = ROOT / 'scripts/report_transfer_pilot.py'
PROTOCOL = dict(schema='cpp-recall-transfer-pilot/v1', id='test/v1',
                skill_id='raii.fundamentals', pause_hours=72, followup_pause_hours=336,
                required_skills=['references.fundamentals', 'oop.objects'],
                preparation=dict(question_id='intro', revision='a' * 64, family_id='scope'),
                control=dict(question_id='control', revision='b' * 64, family_id='scope'))


def event(key, day, **changes):
    row = dict(id=key, direction='cpp', user_id=11, question_id='control',
               revision='b' * 64, family_id='scope', skill_id='raii.fundamentals',
               evidence_type='application', occurred_at=f'2026-11-{day:02d}T12:00:00Z',
               event='answer', attempt_number=1, correct=True, assisted=False, help_known=True)
    row.update(changes)
    return row


def prepared():
    return [event('ref', 1, skill_id='references.fundamentals', question_id='ref'),
            event('oop', 1, skill_id='oop.objects', question_id='oop'),
            event('intro', 2, question_id='intro', revision='a' * 64)]


def snapshot(events):
    return dict(schema='cpp-recall-retrieval-input/v1', observed_until='2026-11-30T12:00:00Z',
                tracking_since='2026-11-01T00:00:00Z', complete=True, events=events)


class TransferPilotReportTest(unittest.TestCase):
    def run_cli(self, document, protocol=PROTOCOL):
        with tempfile.TemporaryDirectory(prefix='transfer-pilot-') as directory:
            config = Path(directory) / 'protocol.json'
            config.write_text(json.dumps(protocol), encoding='utf-8')
            return subprocess.run([sys.executable, str(SCRIPT), '--protocol', str(config)],
                                  input=json.dumps(document), text=True, capture_output=True, timeout=10)

    def report(self, events, **changes):
        document = snapshot(events)
        document.update(changes)
        result = self.run_cli(document)
        self.assertEqual(result.returncode, 0, result.stderr)
        return json.loads(result.stdout)

    def test_exact_three_day_pause_accepts_first_recorded_control_not_mastery(self):
        report = self.report(prepared() + [event('control', 5)])
        self.assertEqual(report['first_recorded'], dict(total=1, correct=1, incorrect=0))
        self.assertEqual(report['followup']['total'], 0)
        self.assertEqual(report['interpretation'], 'recorded_context_candidates_not_mastery')
        self.assertNotIn('user_id', json.dumps(report))
        self.assertNotIn('question_id', json.dumps(report))

    def test_one_second_before_three_days_is_early(self):
        report = self.report(prepared() + [event('control', 5, occurred_at='2026-11-05T11:59:59Z')])
        self.assertEqual(report['outcomes']['early'], 1)
        self.assertEqual(report['first_recorded']['total'], 0)

    def test_first_delayed_failure_is_not_lost_to_later_repair(self):
        report = self.report(prepared() + [event('c1', 5, correct=False),
                                          event('c2', 5, attempt_number=2, occurred_at='2026-11-05T13:00:00Z')])
        self.assertEqual(report['first_recorded'], dict(total=1, correct=0, incorrect=1))
        self.assertEqual(report['outcomes']['repair'], 1)

    def test_assistance_and_repair_are_not_independent(self):
        for fields, category in [({'assisted': True}, 'assisted'), ({'attempt_number': 2}, 'repair')]:
            with self.subTest(fields=fields):
                report = self.report(prepared() + [event('c1', 5, **fields)])
                self.assertEqual(report['outcomes'][category], 1)
                self.assertEqual(report['first_recorded']['total'], 0)

    def test_early_attempt_cannot_become_a_first_context_days_later(self):
        report = self.report(prepared() + [event('early', 2), event('later', 8)])
        self.assertEqual(report['first_recorded']['total'], 0)
        self.assertEqual(report['followup']['total'], 0)
        self.assertEqual(report['outcomes']['previously_exposed'], 1)

    def test_help_before_any_control_answer_is_exposure(self):
        report = self.report(prepared() + [event('help', 4, event='help'), event('c1', 8)])
        self.assertEqual(report['outcomes']['previously_exposed'], 1)
        self.assertEqual(report['first_recorded']['total'], 0)

    def test_intervening_same_skill_activity_restarts_pause(self):
        report = self.report(prepared() + [event('other', 4, question_id='other', family_id='another'),
                                          event('c1', 5)])
        self.assertEqual(report['outcomes']['early'], 1)

    def test_same_time_help_never_counts_even_without_assisted_flag(self):
        report = self.report(prepared() + [event('a-answer', 5), event('z-help', 5, event='help')])
        self.assertEqual(report['first_recorded']['total'], 0)

    def test_exact_fourteen_day_followup_is_familiar_and_separate(self):
        report = self.report(prepared() + [event('c1', 5), event('c2', 19, correct=False)])
        self.assertEqual(report['first_recorded'], dict(total=1, correct=1, incorrect=0))
        self.assertEqual(report['followup'], dict(total=1, correct=0, incorrect=1))

    def test_followup_pause_is_since_last_activity_not_first_control(self):
        report = self.report(prepared() + [event('c1', 5), event('help', 18, event='help'), event('c2', 19)])
        self.assertEqual(report['followup']['total'], 0)
        self.assertEqual(report['outcomes']['early'], 1)

    def test_only_one_followup_per_person_enters_denominator(self):
        report = self.report(prepared() + [event('c1', 5), event('c2', 19),
                                          event('c3', 19, occurred_at='2026-11-19T13:00:00Z')])
        self.assertEqual(report['followup']['total'], 1)

    def test_missing_preparation_is_not_a_transfer_failure(self):
        report = self.report(prepared()[:2] + [event('c1', 5, correct=False)])
        self.assertEqual(report['outcomes']['without_preparation'], 1)
        self.assertEqual(report['first_recorded']['total'], 0)

    def test_prerequisite_gap_is_not_a_transfer_failure(self):
        report = self.report(prepared()[2:] + [event('c1', 5, correct=False)])
        self.assertEqual(report['outcomes']['missing_prerequisites'], 1)
        self.assertEqual(report['first_recorded']['total'], 0)

    def test_same_time_prerequisite_is_not_assumed_to_precede_control(self):
        rows = prepared()[1:] + [event('a-ref', 5, skill_id='references.fundamentals', question_id='ref'),
                                 event('z-control', 5)]
        report = self.report(rows)
        self.assertEqual(report['first_recorded']['total'], 0)

    def test_conflicting_same_time_prerequisite_does_not_depend_on_ids(self):
        for good_id, bad_id in [('a-good', 'z-bad'), ('z-good', 'a-bad')]:
            with self.subTest(good_id=good_id):
                rows = prepared()[1:] + [event(good_id, 1, skill_id='references.fundamentals', question_id='ref'),
                                         event(bad_id, 1, skill_id='references.fundamentals',
                                               question_id='ref', correct=False), event('control', 5)]
                report = self.report(rows)
                self.assertEqual(report['first_recorded']['total'], 0)
                self.assertEqual(report['outcomes']['missing_prerequisites'], 1)

    def test_prerequisite_help_at_same_time_does_not_establish_clean_eligibility(self):
        rows = prepared() + [event('help', 1, event='help', skill_id='references.fundamentals', question_id='ref'),
                             event('control', 5)]
        report = self.report(rows)
        self.assertEqual(report['first_recorded']['total'], 0)
        self.assertEqual(report['outcomes']['missing_prerequisites'], 1)

    def test_no_return_is_absence_of_observation_not_error(self):
        report = self.report(prepared())
        self.assertEqual(report['control_answers'], 0)
        self.assertEqual(report['first_recorded']['incorrect'], 0)

    def test_old_or_unknown_help_tracking_is_not_clean(self):
        for fields, changes in [({'help_known': False}, {}), ({'assisted': None}, {}),
                                ({}, {'tracking_since': None}),
                                ({}, {'tracking_since': '2026-11-06T00:00:00Z'})]:
            with self.subTest(fields=fields, changes=changes):
                report = self.report(prepared() + [event('c1', 5, **fields)], **changes)
                self.assertEqual(report['first_recorded']['total'], 0)

    def test_revision_and_metadata_changes_are_not_the_reviewed_control(self):
        for fields in [{'revision': 'c' * 64}, {'family_id': 'different'},
                       {'skill_id': 'oop.objects'}, {'evidence_type': 'recognition'}]:
            with self.subTest(fields=fields):
                report = self.report(prepared() + [event('c1', 5, **fields)])
                self.assertEqual(report['outcomes']['changed_content'], 1)
                self.assertEqual(report['first_recorded']['total'], 0)

    def test_unknown_activity_and_equal_time_answers_are_not_ordered_by_id(self):
        cases = [[event('unknown', 4, skill_id=None), event('c1', 8)],
                 [event('c1', 5), event('unknown', 5, skill_id=None)],
                 [event('c1', 5), event('c2', 5, correct=False)]]
        for events in cases:
            with self.subTest(events=events):
                report = self.report(prepared() + events)
                self.assertEqual(report['first_recorded']['total'], 0)

    def test_users_and_directions_do_not_share_preparation_or_pause(self):
        report = self.report(prepared() + [event('c1', 5, user_id=12),
                                          event('sd', 5, direction='system_design')])
        self.assertEqual(report['outcomes']['without_preparation'], 1)
        self.assertEqual(report['control_answers'], 1)
        self.assertEqual(report['system_design'], 'not_assessed')

    def test_malformed_or_incomplete_inputs_return_no_partial_report(self):
        cases = [dict(complete=False), dict(events=[event('future', 30, occurred_at='2026-12-01T00:00:00Z')]),
                 dict(events=[event('same', 5), event('same', 8)])]
        for changes in cases:
            with self.subTest(changes=changes):
                document = snapshot(prepared())
                document.update(changes)
                result = self.run_cli(document)
                self.assertEqual(result.returncode, 2)
                self.assertEqual(result.stdout, '')
                self.assertNotIn('future', result.stderr)

    def test_invalid_protocol_is_rejected_not_silently_defaulted(self):
        for changes in [dict(pause_hours=0), dict(followup_pause_hours=72),
                        dict(required_skills=['raii.fundamentals']), dict(schema='wrong')]:
            with self.subTest(changes=changes):
                protocol = dict(PROTOCOL, **changes)
                result = self.run_cli(snapshot([]), protocol)
                self.assertEqual(result.returncode, 2)
                self.assertEqual(result.stdout, '')

    def test_absent_tracking_field_remains_unknown_not_a_traceback(self):
        document = snapshot(prepared() + [event('c1', 5)])
        del document['tracking_since']
        result = self.run_cli(document)
        self.assertEqual(result.returncode, 0, result.stderr)
        report = json.loads(result.stdout)
        self.assertIsNone(report['tracking_since'])
        self.assertEqual(report['first_recorded']['total'], 0)


if __name__ == '__main__':
    unittest.main()
