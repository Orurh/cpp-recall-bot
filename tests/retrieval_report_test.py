"""MEM-01: real offline CLI, synthetic histories, no database or network."""
import json
from pathlib import Path
import subprocess
import sys
import unittest

ROOT = Path(__file__).resolve().parents[1]
SCRIPT = ROOT / 'scripts/report_retrieval.py'


def answer(key, day, **changes):
    value = dict(id=key, direction='cpp', user_id=11, question_id='q1',
                 revision='a' * 64, skill_id='containers.vector', family_id='vector.size',
                 evidence_type='recognition', occurred_at=f'2026-11-{day:02d}T12:00:00Z',
                 event='answer', attempt_number=1, correct=True, assisted=False, help_known=True)
    value.update(changes)
    return value


def snapshot(events):
    return dict(schema='cpp-recall-retrieval-input/v1', observed_until='2026-11-30T00:00:00Z',
                tracking_since='2026-11-01T00:00:00Z', complete=True, events=events)


class RetrievalReportTest(unittest.TestCase):
    def run_cli(self, document, *args):
        return subprocess.run([sys.executable, str(SCRIPT), *args], input=json.dumps(document),
                              text=True, capture_output=True, timeout=10)

    def report(self, events, **changes):
        document = snapshot(events)
        document.update(changes)
        result = self.run_cli(document)
        self.assertEqual(result.returncode, 0, result.stderr)
        return json.loads(result.stdout)

    def test_empty_history_has_no_failure_or_percentage(self):
        report = self.report([])
        self.assertEqual(report['answers'], 0)
        self.assertEqual(report['delayed']['total'], 0)
        self.assertIsNone(report['delayed']['correct_percent'])

    def test_report_carries_the_cutoff_and_tracking_boundary(self):
        report = self.report([])
        self.assertEqual(report.get('observed_until'), '2026-11-30T00:00:00+00:00')
        self.assertEqual(report.get('tracking_since'), '2026-11-01T00:00:00+00:00')

    def test_outcomes_partition_all_answers_and_empty_return_is_not_failure(self):
        report = self.report([answer('a1', 2), answer('a2', 8, assisted=True),
                              answer('a3', 9, attempt_number=2), answer('a4', 12)])
        self.assertEqual(sum(report['outcomes'].values()), 4)
        self.assertEqual(sum(report['delayed']['familiarity'].values()), report['delayed']['total'])
        self.assertEqual(report['delayed']['incorrect'], 0)

    def test_equal_timestamp_help_is_conservatively_processed_before_answer(self):
        report = self.report([answer('a1', 2), answer('a2', 8), answer('h1', 8, event='help')])
        self.assertEqual(report['outcomes']['short_pause'], 1)
        self.assertEqual(report['delayed']['total'], 0)

    def test_unknown_tracking_or_null_help_never_creates_clean_evidence(self):
        for changes in ({'tracking_since': None}, {}):
            with self.subTest(changes=changes):
                report = self.report([answer('a1', 2), answer('a2', 8, assisted=None)], **changes)
                self.assertEqual(report['delayed']['total'], 0)

    def test_invalid_pause_threshold_is_rejected(self):
        for value in ('0', '-1', '721', '1.5'):
            result = self.run_cli(snapshot([]), '--pause-hours', value)
            self.assertEqual(result.returncode, 2)
            self.assertEqual(result.stdout, '')

    def test_familiar_delayed_failure_is_an_observation_not_mastery_loss(self):
        report = self.report([answer('a1', 2), answer('a2', 8, correct=False)])
        self.assertEqual(report['outcomes']['first_observation'], 1)
        self.assertEqual(report['delayed']['total'], 1)
        self.assertEqual(report['delayed']['incorrect'], 1)
        self.assertEqual(report['delayed']['familiarity']['familiar_card'], 1)
        self.assertNotIn('mastery', report)

    def test_assisted_success_is_not_a_delayed_unaided_success(self):
        report = self.report([answer('a1', 2), answer('a2', 8, assisted=True)])
        self.assertEqual(report['outcomes']['assisted'], 1)
        self.assertEqual(report['delayed']['total'], 0)

    def test_repair_is_not_a_first_independent_answer_even_after_days(self):
        report = self.report([answer('a1', 2, correct=False), answer('a2', 8, attempt_number=2)])
        self.assertEqual(report['outcomes']['repair'], 1)
        self.assertEqual(report['delayed']['total'], 0)

    def test_same_day_correct_answer_is_not_delayed_retrieval(self):
        report = self.report([answer('a1', 2), answer('a2', 2, occurred_at='2026-11-02T13:00:00Z')])
        self.assertEqual(report['outcomes']['short_pause'], 1)
        self.assertEqual(report['delayed']['total'], 0)

    def test_help_without_answer_restarts_the_pause(self):
        help_event = answer('h1', 7, event='help', attempt_number=None, correct=None,
                            assisted=True, occurred_at='2026-11-07T11:00:00Z')
        report = self.report([answer('a1', 2), help_event,
                              answer('a2', 7, occurred_at='2026-11-07T12:00:00Z')])
        self.assertEqual(report['help_events'], 1)
        self.assertEqual(report['outcomes']['short_pause'], 1)
        self.assertEqual(report['delayed']['total'], 0)

    def test_intervening_other_family_of_same_skill_restarts_the_pause(self):
        report = self.report([answer('a1', 2), answer('a2', 7, question_id='q2', family_id='vector.capacity'),
                              answer('a3', 7, occurred_at='2026-11-07T13:00:00Z')])
        self.assertEqual(report['outcomes']['short_pause'], 1)
        self.assertEqual(report['delayed']['total'], 1)

    def test_family_variant_is_not_an_independent_new_context(self):
        report = self.report([answer('a1', 2), answer('a2', 8, question_id='q2')])
        self.assertEqual(report['delayed']['familiarity']['family_variant'], 1)
        self.assertEqual(report['delayed']['correct'], 1)

    def test_other_family_and_changed_revision_are_not_automatically_transfer(self):
        report = self.report([answer('a1', 2), answer('a2', 8, family_id='vector.capacity', question_id='q2'),
                              answer('a3', 12, revision='b' * 64)])
        self.assertEqual(report['delayed']['familiarity']['other_family_unverified_context'], 1)
        self.assertEqual(report['delayed']['familiarity']['changed_revision'], 1)

    def test_unknown_legacy_assistance_is_not_treated_as_false(self):
        report = self.report([answer('a1', 2), answer('a2', 8, help_known=False)])
        self.assertEqual(report['outcomes']['unknown_assistance'], 1)
        self.assertEqual(report['delayed']['total'], 0)

    def test_before_tracking_is_unknown_even_if_help_flag_says_false(self):
        report = self.report([answer('a1', 2), answer('a2', 8)], tracking_since='2026-11-10T00:00:00Z')
        self.assertEqual(report['outcomes']['unknown_assistance'], 2)
        self.assertEqual(report['delayed']['total'], 0)

    def test_gap_crossing_start_of_tracking_is_not_claimed_to_be_observed(self):
        report = self.report([answer('a1', 2), answer('a2', 12)], tracking_since='2026-11-10T00:00:00Z')
        self.assertEqual(report['outcomes']['insufficient_history'], 1)
        self.assertEqual(report['delayed']['total'], 0)

    def test_unknown_metadata_blocks_false_long_gap_for_the_user(self):
        report = self.report([answer('a1', 2), answer('h1', 7, event='help', skill_id=None), answer('a2', 8)])
        self.assertEqual(report['outcomes']['insufficient_history'], 1)
        self.assertEqual(report['delayed']['total'], 0)

    def test_equal_timestamp_unknown_activity_blocks_gap_independent_of_event_id(self):
        for unknown_id in ('a_unknown', 'z_unknown'):
            with self.subTest(unknown_id=unknown_id):
                report = self.report([answer('initial', 2), answer('m_candidate', 8),
                                      answer(unknown_id, 8, skill_id=None)])
                self.assertEqual(report['outcomes']['insufficient_history'], 1)
                self.assertEqual(report['delayed']['total'], 0)

    def test_simultaneous_answers_with_unknown_order_do_not_choose_a_winner_by_id(self):
        for correct_id, wrong_id in (('a_correct', 'z_wrong'), ('z_correct', 'a_wrong')):
            with self.subTest(correct_id=correct_id):
                report = self.report([answer('initial', 2), answer(correct_id, 8),
                                      answer(wrong_id, 8, question_id='q2', correct=False)])
                self.assertEqual(report['outcomes']['insufficient_history'], 2)
                self.assertEqual(report['delayed']['total'], 0)

    def test_user_skill_and_domain_are_independent(self):
        report = self.report([answer('a1', 2), answer('a2', 8, user_id=22),
                              answer('a3', 8, skill_id='oop.virtual'),
                              dict(direction='system_design')])
        self.assertEqual(report['outcomes']['first_observation'], 3)
        self.assertEqual(report['excluded_other_directions'], 1)
        self.assertEqual(report['delayed']['total'], 0)

    def test_unordered_input_is_processed_chronologically(self):
        report = self.report([answer('a2', 8), answer('a1', 2)])
        self.assertEqual(report['delayed']['total'], 1)

    def test_threshold_is_explicit_and_boundary_inclusive(self):
        document = snapshot([answer('a1', 2), answer('a2', 3)])
        first = self.run_cli(document, '--pause-hours', '24')
        second = self.run_cli(document, '--pause-hours', '48')
        self.assertEqual(first.returncode, 0, first.stderr)
        self.assertEqual(second.returncode, 0, second.stderr)
        self.assertEqual(json.loads(first.stdout)['delayed']['total'], 1)
        self.assertEqual(json.loads(second.stdout)['delayed']['total'], 0)

    def test_incomplete_export_fails_instead_of_publishing_biased_counts(self):
        result = self.run_cli(dict(snapshot([]), complete=False))
        self.assertEqual(result.returncode, 2)
        self.assertEqual(result.stdout, '')
        self.assertIn('неполный', result.stderr.lower())

    def test_invalid_or_duplicate_data_is_rejected_without_echoing_records(self):
        good = snapshot([answer('a1', 2)])
        broken = [dict(good, schema='unknown'), dict(good, events=good['events'] * 2),
                  snapshot([answer('a1', 2, correct='false')]),
                  snapshot([answer('a1', 2, occurred_at='not-a-time')]),
                  snapshot([answer('a1', 2, occurred_at='2026-12-01T12:00:00Z')])]
        for doc in broken:
            with self.subTest(document=doc):
                result = self.run_cli(doc)
                self.assertEqual(result.returncode, 2)
                self.assertEqual(result.stdout, '')
                self.assertIn('Некорректные', result.stderr)
                self.assertNotIn('containers.vector', result.stderr)

    def test_output_omits_user_ids_answers_and_question_identifiers(self):
        report = self.report([answer('a1', 2), answer('a2', 8)])
        encoded = json.dumps(report)
        for forbidden in ('user_id', 'question_id', 'q1', 'vector.size'):
            self.assertNotIn(forbidden, encoded)


if __name__ == '__main__':
    unittest.main(verbosity=2)
