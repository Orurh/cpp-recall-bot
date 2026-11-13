"""Exercise the real pilot CLI on contact-aware synthetic histories, not learners."""
import json
from pathlib import Path
import subprocess
import sys
import unittest

from transfer_pilot_report_test import PROTOCOL
import transfer_pilot_report_test as legacy

ROOT = Path(__file__).resolve().parents[1]


def event(key, day, session=100, **changes):
    row = dict(id=key, direction='cpp', user_id=11, session_id=session,
               question_id='control', revision='b' * 64, family_id='scope', skill_id='raii.fundamentals',
               evidence_type='application', occurred_at=f'2026-11-{day:02d}T12:00:00Z',
               event='answer', attempt_number=1, correct=True, assisted=False, help_known=True)
    row.update(changes)
    return row


def offer(key, day, session=100, **changes):
    return event(key, day, session, event='contact', kind='question_delivered',
                 correct=None, assisted=None, help_known=None, **changes)


def prepared():
    return [event('ref', 1, 1, skill_id='references.fundamentals', question_id='ref'),
            event('oop', 1, 2, skill_id='oop.objects', question_id='oop'),
            event('intro', 2, 3, question_id='intro', revision='a' * 64)]


def trial(day=5, session=100, correct=True):
    return [offer(f'offer-{session}', day, session),
            event(f'answer-{session}', day, session, correct=correct,
                  occurred_at=f'2026-11-{day:02d}T13:00:00Z')]


def snapshot(rows, **changes):
    doc = dict(schema='cpp-recall-study-input/v1', observed_until='2026-12-31T12:00:00Z',
               tracking_since='2026-11-01T00:00:00Z', contacts_since='2026-11-01T00:00:00Z',
               complete=True, read_only=True, events=rows)
    doc.update(changes)
    return doc


class TransferContactsReportTest(unittest.TestCase):
    run_cli = legacy.TransferPilotReportTest.run_cli

    def report(self, rows, **changes):
        result = self.run_cli(snapshot(rows, **changes), PROTOCOL)
        self.assertEqual(result.returncode, 0, result.stderr)
        return json.loads(result.stdout)

    def test_due_first_delivery_is_not_invalidated_by_its_own_contact(self):
        report = self.report(prepared() + trial())
        self.assertEqual(report['schema'], 'cpp-recall-transfer-pilot-report/v2')
        self.assertEqual(report['first_recorded'], dict(total=1, correct=1, incorrect=0))
        self.assertEqual(report['unrecorded_exposure'], 'unknown')
        self.assertNotIn('user_id', json.dumps(report))
        self.assertNotIn('question_id', json.dumps(report))

    def test_days_spent_with_an_early_open_card_do_not_create_delayed_retrieval(self):
        rows = prepared() + [offer('opened-early', 2, occurred_at='2026-11-02T13:00:00Z'),
                              event('answered-days-later', 8)]
        report = self.report(rows)
        self.assertEqual(report['outcomes']['early'], 1)
        self.assertEqual(report['first_recorded']['total'], 0)

    def test_one_second_before_72_hours_is_early_even_when_answer_is_later(self):
        rows = prepared() + [offer('early', 5, occurred_at='2026-11-05T11:59:59Z'), event('answer', 8)]
        self.assertEqual(self.report(rows)['outcomes']['early'], 1)

    def test_repeated_delivery_same_encounter_is_not_a_new_trial_or_zero_pause(self):
        rows = prepared() + trial()
        rows.append(offer('retry', 5, occurred_at='2026-11-05T12:30:00Z'))
        report = self.report(list(reversed(rows)))
        self.assertEqual(report['first_recorded']['total'], 1)
        self.assertEqual(report['control_encounters'], 1)

    def test_opened_without_answer_consumes_first_exposure_not_a_failure(self):
        report = self.report(prepared() + [offer('abandoned', 3, session=99)] + trial(8))
        self.assertEqual(report['outcomes']['previously_exposed'], 1)
        self.assertEqual(report['unanswered_encounters'], 1)
        self.assertEqual(report['first_recorded']['incorrect'], 0)

    def test_help_even_with_false_answer_flag_excludes_clean_result(self):
        for kind in ('reference_opened', 'theory_opened', 'answer_revealed'):
            with self.subTest(kind=kind):
                rows = prepared() + trial() + [event('help', 5, event='contact', kind=kind,
                    occurred_at='2026-11-05T12:30:00Z', correct=None, assisted=None, help_known=None)]
                report = self.report(rows)
                self.assertEqual(report['outcomes']['assisted'], 1)
                self.assertEqual(report['first_recorded']['total'], 0)

    def test_help_after_first_wrong_answer_does_not_erase_independent_failure(self):
        rows = prepared() + trial(correct=False) + [event('reveal', 5, event='contact', kind='answer_revealed',
            occurred_at='2026-11-05T14:00:00Z', correct=None, assisted=None, help_known=None)]
        report = self.report(rows)
        self.assertEqual(report['first_recorded'], dict(total=1, correct=0, incorrect=1))

    def test_legacy_help_in_current_training_is_assistance_not_merely_other_activity(self):
        rows = prepared() + trial() + [event('legacy-help', 5, event='help', attempt_number=None,
                                            occurred_at='2026-11-05T12:30:00Z')]
        self.assertEqual(self.report(rows)['outcomes']['assisted'], 1)

    def test_same_time_prerequisite_reveal_in_other_encounter_cannot_become_clean_by_id(self):
        for first, second in [('a', 'z'), ('z', 'a')]:
            rows = prepared()[1:] + trial() + [event(first, 1, 1, skill_id='references.fundamentals', question_id='ref'),
                event(second, 1, 999, skill_id='references.fundamentals', question_id='other-ref',
                      event='contact', kind='answer_revealed', correct=None, assisted=None, help_known=None)]
            self.assertEqual(self.report(rows)['outcomes']['missing_prerequisites'], 1)

    def test_other_same_skill_activity_after_offer_interrupts_independence(self):
        rows = prepared() + trial() + [offer('other-card', 5, session=999, question_id='other',
                                           occurred_at='2026-11-05T12:30:00Z')]
        report = self.report(rows)
        self.assertEqual(report['outcomes']['intervening_activity'], 1)

    def test_other_skill_offer_does_not_interrupt_target_skill(self):
        rows = prepared() + trial() + [offer('other-skill', 5, session=999, skill_id='containers.vector',
                                            question_id='other', occurred_at='2026-11-05T12:30:00Z')]
        self.assertEqual(self.report(rows)['first_recorded']['total'], 1)

    def test_old_feedback_reopening_before_control_restarts_pause(self):
        rows = prepared() + trial() + [event('old-feedback', 4, 3, question_id='intro', revision='a' * 64,
            event='contact', kind='answer_revealed', correct=None, assisted=None, help_known=None)]
        self.assertEqual(self.report(rows)['outcomes']['early'], 1)

    def test_new_encounter_after_336_hours_is_familiar_followup(self):
        report = self.report(prepared() + trial() + trial(19, session=200, correct=False))
        # First answer was Nov5 13:00, so Nov19 12:00 is one hour too early.
        self.assertEqual(report['followup']['total'], 0)
        rows = prepared() + trial() + [offer('follow', 19, session=200, occurred_at='2026-11-19T13:00:00Z'),
                                      event('follow-answer', 20, session=200, correct=False)]
        self.assertEqual(self.report(rows)['followup'], dict(total=1, correct=0, incorrect=1))

    def test_reopening_first_encounter_two_weeks_later_is_not_followup(self):
        rows = prepared() + trial() + [offer('reopen', 20)]
        report = self.report(rows)
        self.assertEqual(report['control_encounters'], 1)
        self.assertEqual(report['followup']['total'], 0)

    def test_answer_without_delivery_is_not_a_new_clean_control(self):
        report = self.report(prepared() + [event('answer', 5)])
        self.assertEqual(report['outcomes']['missing_delivery'], 1)
        self.assertEqual(report['first_recorded']['total'], 0)

    def test_legacy_help_without_encounter_also_consumes_control_exposure(self):
        rows = prepared() + trial(8) + [event('legacy', 3, session=99, event='help', attempt_number=None)]
        self.assertEqual(self.report(rows)['outcomes']['previously_exposed'], 1)

    def test_missing_contact_boundary_or_prior_outside_it_cannot_certify_pause(self):
        for boundary in (None, '2026-11-03T00:00:00Z'):
            with self.subTest(boundary=boundary):
                report = self.report(prepared() + trial(), contacts_since=boundary)
                self.assertEqual(report['first_recorded']['total'], 0)

    def test_unknown_metadata_between_preparation_and_control_blocks_claim(self):
        rows = prepared() + trial() + [offer('unknown', 4, 90, question_id='old', revision=None, skill_id=None)]
        self.assertEqual(self.report(rows)['outcomes']['insufficient_history'], 1)

    def test_equal_time_other_activity_cannot_depend_on_event_ids(self):
        for first, second in [('a', 'z'), ('z', 'a')]:
            rows = prepared() + [offer(first, 5), event('answer', 5, occurred_at='2026-11-05T13:00:00Z'),
                                offer(second, 5, 999, question_id='other')]
            self.assertEqual(self.report(rows)['outcomes']['insufficient_history'], 1)

    def test_equal_time_other_answer_cannot_depend_on_event_ids(self):
        for answer_id, other_id in [('a', 'z'), ('z', 'a')]:
            for fields in ({'skill_id': 'raii.fundamentals'}, {'skill_id': None, 'revision': None}):
                with self.subTest(answer_id=answer_id, fields=fields):
                    rows = prepared() + [offer('offer', 5), event(answer_id, 5, occurred_at='2026-11-05T13:00:00Z'),
                        event(other_id, 5, 999, question_id='other',
                              occurred_at='2026-11-05T13:00:00Z', **fields)]
                    report = self.report(rows)
                    self.assertEqual(report['outcomes']['insufficient_history'], 1)
                    self.assertEqual(report['first_recorded']['total'], 0)

    def test_duplicate_answer_encounter_is_ambiguous_not_two_observations(self):
        rows = prepared() + trial() + [event('conflict', 5, correct=False, occurred_at='2026-11-05T14:00:00Z')]
        report = self.report(rows)
        self.assertEqual(report['outcomes']['ambiguous_encounter'], 1)
        self.assertEqual(report['first_recorded']['total'], 0)

    def test_missing_preparation_prerequisites_and_repair_stay_distinct(self):
        for rows, expected in [(trial(), 'without_preparation'),
                               (prepared()[2:] + trial(), 'missing_prerequisites'),
                               (prepared() + [offer('repair-offer', 5, attempt_number=2),
                                              event('repair-answer', 5, attempt_number=2)], 'repair')]:
            with self.subTest(expected=expected):
                self.assertEqual(self.report(rows)['outcomes'][expected], 1)

    def test_partial_future_duplicate_and_malformed_input_fail_closed(self):
        for changes in ({'complete': False}, {'events': [event('future', 5, occurred_at='2027-01-01T00:00:00Z')]},
                        {'events': [event('same', 5), event('same', 6)]},
                        {'events': [offer('bad', 5, session_id=False)]}, {'read_only': False}):
            with self.subTest(changes=changes):
                result = self.run_cli(snapshot(prepared() + trial(), **changes))
                self.assertEqual(result.returncode, 2)
                self.assertEqual(result.stdout, '')


if __name__ == '__main__':
    unittest.main(verbosity=2)
