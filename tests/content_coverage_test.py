"""Fixtures test structural counts, not learners or C++ truth."""
from copy import deepcopy
import importlib.util
import json
from pathlib import Path
import subprocess
import sys
import unittest

ROOT = Path(__file__).resolve().parent.parent
spec = importlib.util.spec_from_file_location('coverage_report', ROOT / 'scripts/report_content_coverage.py')
coverage = importlib.util.module_from_spec(spec)
spec.loader.exec_module(coverage)
EXECUTABLE = Path(sys.argv.pop(1)).resolve() if len(sys.argv) > 1 else ROOT / 'build/content_check'


def card(qid='one', family='family', level=2, **changes):
    return {'id': qid, 'public_id': f'CR-{qid}', 'content_revision': 'a' * 64, 'topic': 'base',
            'standard': 'C++23', 'skill_id': 'base.a', 'family_id': family, 'level': level,
            'evidence_type': 'application', 'transfer': True, 'references': [], **changes}


def fixture():
    return {'schema_version': 1, 'topics': [{'id': 'base', 'title': 'База'}, {'id': 'extra', 'title': 'Дополнительно'}],
            'skills': [{'id': 'base.a', 'title': 'Навык A', 'topic': 'base', 'assessable': True, 'prerequisites': []},
                       {'id': 'base.empty', 'title': 'Без карточек', 'topic': 'base', 'assessable': False, 'prerequisites': ['base.a']}],
            'questions': [card()], 'references': [], 'theory': [], 'routes': []}


class CoverageTest(unittest.TestCase):
    def test_variants_and_levels_do_not_multiply_families(self):
        data = fixture()
        data['questions'] += [card('two', level=6), card('three', family='other', level=6)]
        report = coverage.build_report(data, {})
        self.assertEqual((report['totals']['cards'], report['totals']['families']), (3, 2))
        self.assertEqual(report['totals']['tagged_transfer_families'], 2)
        row = report['skills'][0]
        self.assertEqual(row['levels'][1]['families'], 1)
        self.assertEqual(row['levels'][5]['families'], 2)
        self.assertEqual(len(row['levels']), 10)
        self.assertEqual(row['accepted_revisions'], 0)

    def test_zero_card_skills_and_empty_topics_remain_visible(self):
        report = coverage.build_report(fixture(), {})
        empty = next(row for row in report['skills'] if row['id'] == 'base.empty')
        self.assertEqual(empty['gaps'], ['no_cards'])
        self.assertEqual(empty['families'], 0)
        self.assertEqual(report['topics'][1]['cards'], 0)
        self.assertIn('нет карточек', coverage.markdown(report))

    def test_definition_help_and_theory_are_not_inferred_to_be_intro_or_transfer(self):
        data = fixture()
        data['references'] = [{'id': 'help'}]
        data['theory'] = [{'id': 'after-error', 'skill_id': 'base.a'}]
        data['questions'] = [card(level=1, references=['help'], evidence_type='recognition', transfer=False, kind='theory')]
        row = coverage.build_report(data, {})['skills'][0]
        self.assertEqual(row['reference_cards'], 1)
        self.assertEqual(row['theory_block_ids'], ['after-error'])
        self.assertEqual(row['route_practice_cards'], 0)
        self.assertEqual(row['application_families'], 0)
        self.assertEqual(row['introduction_status'], 'not_assessed')
        self.assertIn('no_route_practice', row['gaps'])

    def test_route_stages_count_the_card_skill_not_the_whole_target(self):
        data = fixture()
        data['questions'] += [card('two', family='other')]
        data['routes'] = [{'id': 'path', 'retention_skill_id': 'base.empty', 'stages': [
            {'id': 'intro', 'kind': 'practice', 'question_ids': ['one']},
            {'id': 'apply', 'kind': 'independent', 'question_ids': ['two']}]}]
        rows = coverage.build_report(data, {})['skills']
        self.assertEqual(rows[0]['route_practice_cards'], 1)
        self.assertEqual(rows[0]['route_independent_cards'], 1)
        self.assertEqual(rows[1]['route_independent_cards'], 0)

    def test_revision_acceptance_and_independence_are_separate(self):
        data = fixture()
        q = data['questions'][0]
        record = {**q, 'question_family_id': q['family_id'], 'decision': 'accepted', 'independent': False}
        reviews = {'one': record}
        report = coverage.build_report(data, reviews)
        self.assertEqual(report['totals']['accepted_revisions'], 1)
        self.assertEqual(report['totals']['independent_reviews'], 0)
        record['content_revision'] = 'b' * 64
        record['independent'] = True
        report = coverage.build_report(data, reviews)
        self.assertEqual(report['totals']['accepted_revisions'], 0)
        self.assertEqual(report['totals']['independent_reviews'], 0)
        self.assertEqual(report['totals']['review_states'], {'stale': 1})
        record['content_revision'] = q['content_revision']
        record['decision'] = 'rejected'
        self.assertEqual(coverage.build_report(data, reviews)['totals']['accepted_revisions'], 0)

    def test_filters_keep_bank_total_and_reject_typos(self):
        data = fixture()
        report = coverage.build_report(data, {}, skill='base.empty')
        self.assertEqual(report['totals']['cards'], 0)
        self.assertEqual(report['bank_totals']['cards'], 1)
        self.assertEqual(len(report['skills']), 1)
        for args in ({'topic': 'typo'}, {'skill': 'typo'}, {'topic': 'extra', 'skill': 'base.a'}):
            with self.assertRaises(ValueError):
                coverage.build_report(data, {}, **args)

    def test_orphaned_review_is_not_silently_ignored(self):
        with self.assertRaises(ValueError):
            coverage.build_report(fixture(), {'unknown': {'decision': 'accepted'}})

    def test_invalid_catalog_fails_instead_of_rendering_false_zero(self):
        cases = []
        for field, value in [('level', 11), ('references', ['unknown']), ('transfer', 'yes'), ('skill_id', 'unknown')]:
            data = fixture()
            data['questions'][0][field] = value
            cases.append(data)
        data = fixture()
        data['questions'].append(deepcopy(data['questions'][0]))
        cases.append(data)
        for data in cases:
            with self.assertRaises(ValueError):
                coverage.build_report(data, {})

    def test_deterministic_non_mutating_report(self):
        data = fixture()
        before = deepcopy(data)
        first = coverage.build_report(data, {})
        self.assertEqual(first, coverage.build_report(data, {}))
        self.assertEqual(data, before)
        self.assertIn('не приёмка беты', coverage.markdown(first))

    def test_real_catalog_uses_current_bindings_and_no_answer_dump(self):
        process = subprocess.run([str(EXECUTABLE), '--coverage-json'], cwd=ROOT,
                                 check=True, capture_output=True, text=True)
        catalog = json.loads(process.stdout)
        report = coverage.build_report(catalog, coverage.load_reviews(ROOT / 'docs/reviews/content'))
        questions = catalog['questions']
        self.assertEqual(report['totals']['cards'], len(questions))
        self.assertEqual(report['totals']['families'], len({q['family_id'] for q in questions}))
        self.assertEqual(report['totals']['reference_cards'], sum(bool(q['references']) for q in questions))
        self.assertEqual(report['totals']['accepted_revisions'], len(questions))
        self.assertEqual(sum(row['cards'] for row in report['skills']), len(questions))
        self.assertEqual(sum(row['assessable'] for row in report['skills']), 20)
        for q in report['questions']:
            self.assertFalse(set(q) & {'text', 'code', 'task_options', 'answer', 'explanation', 'accepted_answers', 'correct_option_id'})
            self.assertFalse(q['id'].startswith('PC-'))
        alias = next(q for q in questions if q['id'] == 'sorted_snapshot_key_alias_completion')
        self.assertTrue(alias['references'])
        focused = coverage.build_report(catalog, {}, skill=alias['skill_id'])
        self.assertIn(alias['public_id'], coverage.markdown(focused))

    def test_cli_unknown_skill_and_missing_binary_fail(self):
        for flags in (['--skill', 'nonexistent'], ['--content-check', '/nonexistent/content_check']):
            result = subprocess.run([sys.executable, str(ROOT / 'scripts/report_content_coverage.py'),
                                     '--content-check', str(EXECUTABLE), *flags], cwd=ROOT,
                                    capture_output=True, text=True)
            self.assertEqual(result.returncode, 2)
            self.assertEqual(result.stdout, '')
            self.assertIn('Отчёт не построен', result.stderr)


if __name__ == '__main__':
    unittest.main()
