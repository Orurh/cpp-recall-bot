"""Validate inactive pilot material. Never run the intentionally undefined program."""
import copy
import hashlib
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[1]
COMPILER = sys.argv.pop(1) if len(sys.argv) > 1 else 'c++'


def revision(tasks):
    return hashlib.sha256(json.dumps(tasks, ensure_ascii=False, sort_keys=True,
                                     separators=(',', ':')).encode()).hexdigest()


class PilotControlTasksTest(unittest.TestCase):
    def setUp(self):
        self.document = json.loads((ROOT / 'docs/pilot/control_tasks.json').read_text())

    def test_metadata_and_isolation(self):
        doc = self.document
        self.assertEqual(doc['schema_version'], 1)
        self.assertEqual(doc['status'], 'prepared_not_active')
        self.assertIs(doc['runtime_enabled'], False)
        self.assertEqual(doc['grading'], 'manual')
        self.assertEqual(doc['standard_document'], 'N4950')
        self.assertEqual(doc['reviewer']['mode'], 'self_review')
        self.assertIs(doc['reviewer']['independent'], False)
        self.assertEqual(revision(doc['tasks']), doc['reviewed_tasks_revision'],
                         'Pilot content changed: repeat source/pedagogical review before updating the digest')
        changed = copy.deepcopy(doc['tasks'])
        changed[0]['prompt'] += ' changed'
        self.assertNotEqual(revision(changed), doc['reviewed_tasks_revision'])
        bank = {q['id']: q for p in (ROOT / 'content/questions').rglob('*.json')
                for q in [json.loads(p.read_text())]}
        ids = [task['id'] for task in doc['tasks']]
        self.assertEqual(set(ids), {'PC-L01', 'PC-L02', 'PC-L03', 'PC-T01', 'PC-T02', 'PC-T03'})
        self.assertEqual(len(ids), len(set(ids)))
        self.assertTrue(set(ids).isdisjoint(bank))
        families = set()
        for task in doc['tasks']:
            with self.subTest(task=task['id']):
                self.assertNotIn('public_id', task)
                self.assertTrue(task['candidate_family_id'].startswith('pilot.'))
                self.assertNotIn(task['candidate_family_id'], families)
                families.add(task['candidate_family_id'])
                self.assertIn(task['route'], {'lifetime_raii', 'templates'})
                self.assertIn(task['minimum_level'], range(1, 11))
                for field in ('prerequisites', 'assessed_rule', 'novel_context', 'prompt', 'code',
                              'rubric', 'reference_ids', 'sources', 'learning_ids'):
                    self.assertTrue(task[field], field)
                self.assertTrue(all(url.startswith('https://timsong-cpp.github.io/cppwp/n4950/')
                                    for url in task['sources']))
                self.assertTrue(all(id in bank for id in task['learning_ids']))
                self.assertTrue(any(bank[id]['skill_id'] == task['skill_id']
                                    for id in task['learning_ids']))
        dockerfile = (ROOT / 'Dockerfile').read_text()
        self.assertIn('COPY docs/miniapp ./docs/miniapp', dockerfile)
        self.assertNotRegex(dockerfile, r'(?m)^COPY\s+docs(?:/|\s)\s*\.')

    def test_reviewed_programs(self):
        # Recorded during manual source review, not obtained from the answer field.
        expected = {'PC-L02': '11 0', 'PC-L03': 'BCDA', 'PC-T01': '246',
                    'PC-T02': '0', 'PC-T03': '50 40'}
        with tempfile.TemporaryDirectory(prefix='cpp-recall-pilot-check-') as directory:
            for task in self.document['tasks']:
                with self.subTest(task=task['id']):
                    source = Path(directory) / (task['id'] + '.cc')
                    binary = Path(directory) / task['id']
                    source.write_text(task['code'])
                    command = [COMPILER, '-std=c++23', '-Wall', '-Wextra', '-pedantic-errors', str(source)]
                    if task['id'] == 'PC-L01':
                        self.assertEqual(task['verification'], 'compile_only_undefined_behavior')
                        self.assertEqual(task['expected_classification'], 'undefined_behavior')
                        self.assertNotIn('expected_stdout', task)
                        subprocess.run(command + ['-fsyntax-only'], check=True,
                                       capture_output=True, timeout=40)
                        continue  # No executable is produced or run for the UB task.
                    self.assertEqual(task['verification'], 'run_stdout')
                    self.assertEqual(task['expected_stdout'], expected[task['id']])
                    subprocess.run(command + ['-o', str(binary)], check=True,
                                   capture_output=True, timeout=40)
                    result = subprocess.run([str(binary)], capture_output=True, text=True,
                                            check=True, timeout=5)
                    self.assertEqual(result.stdout, expected[task['id']])
                    if task['id'] == 'PC-L02':
                        source.write_text(task['solution_code'])
                        subprocess.run(command + ['-o', str(binary)], check=True,
                                       capture_output=True, timeout=40)
                        result = subprocess.run([str(binary)], capture_output=True, text=True,
                                                check=True, timeout=5)
                        self.assertEqual(task['solution_stdout'], '11 1')
                        self.assertEqual(result.stdout, '11 1')


if __name__ == '__main__':
    unittest.main()
