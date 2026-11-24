"""Exercise authoring validation and real generated C++ bytes, without services."""
from copy import deepcopy
import importlib.util
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[1]
SCRIPT = ROOT / 'scripts/generate_learning_content.py'
COMPILER = sys.argv.pop(1) if len(sys.argv) > 1 else 'c++'


def fixture():
    article = dict(catalog_order=0, id='sample', title='Title', principle='Rule',
                   mechanism='Mechanism', example='Пример "\\\n?9', common_mistake='Mistake',
                   sources=['https://timsong-cpp.github.io/cppwp/n4950/basic.life'],
                   source_kind='cpp_standard')
    return dict(schema_version=1, skill_id='lifetime.objects',
                theory=[dict(article, primary=True)], references=[dict(article)],
                theory_bindings=[dict(catalog_order=0, question_family_id='family', theory_id='sample')],
                reference_bindings=[dict(catalog_order=0, question_id='card',
                                         content_revision='a' * 64, reference_ids=['sample'])])


class GeneratorTest(unittest.TestCase):
    def setUp(self):
        self.assertTrue(SCRIPT.is_file(), 'build-time learning content generator is missing')
        spec = importlib.util.spec_from_file_location('learning_generator', SCRIPT)
        self.generator = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(self.generator)
        self.directory = tempfile.TemporaryDirectory(prefix='cpp-recall-learning-')
        self.addCleanup(self.directory.cleanup)
        self.root = Path(self.directory.name)
        self.path = self.root / 'topics/lifetime/skills/lifetime.objects/materials.json'
        self.path.parent.mkdir(parents=True)
        self.skills = {'lifetime.objects': 'lifetime', 'core.scope': 'language_core'}
        self.questions = {'card': {'skill_id': 'lifetime.objects', 'question_family_id': 'family',
                                  'content_revision': 'a' * 64}}

    def load(self, value):
        self.path.write_text(json.dumps(value, ensure_ascii=False), encoding='utf-8')
        return self.generator.load_materials(self.root, self.skills, self.questions)

    def test_compiled_literals_preserve_unicode_quotes_newlines_and_order(self):
        data = self.load(fixture())
        output = self.root / 'generated'
        self.generator.generate(data, output)
        before = {p.name: p.read_bytes() for p in output.iterdir()}
        self.generator.generate(data, output)
        self.assertEqual(before, {p.name: p.read_bytes() for p in output.iterdir()})
        source = self.root / 'bytes.cc'
        source.write_text('''#include <array>
#include "learning/theory_catalog.h"
#include "learning/reference_catalog.h"
#include <iostream>
namespace {
#include "theory_catalog_data.inc"
#include "reference_catalog_data.inc"
}
int main() {
  std::cout << kTheoryBlocks.front().example << "|" << kEntries.front().example;
  return kTheoryBindings[0].skill_id == "lifetime.objects" &&
      kBindings.front().content_revision == std::string(64, 'a') ? 0 : 1;
}
''', encoding='utf-8')
        binary = self.root / 'bytes'
        compiled = subprocess.run([COMPILER, '-std=c++23', '-pedantic-errors', '-I' + str(ROOT / 'src'),
                                   '-I' + str(output), str(source), '-o', str(binary)],
                                  capture_output=True, text=True, check=False)
        self.assertEqual(compiled.returncode, 0, compiled.stderr)
        actual = subprocess.run([str(binary)], check=True, capture_output=True).stdout
        self.assertEqual(actual, 'Пример "\\\n?9|Пример "\\\n?9'.encode())

    def test_explicit_global_order_beats_skill_path_order(self):
        first = fixture()
        first['theory'][0]['catalog_order'] = 1
        second = dict(schema_version=1, skill_id='core.scope', theory=[dict(first['theory'][0],
                      catalog_order=0, id='earlier')], references=[], theory_bindings=[], reference_bindings=[])
        other = self.root / 'topics/language_core/skills/core.scope/materials.json'
        other.parent.mkdir(parents=True)
        other.write_text(json.dumps(second), encoding='utf-8')
        data = self.load(first)
        self.assertEqual([x['id'] for x in data['theory']], ['earlier', 'sample'])

    def test_reviewed_additional_bundle_can_append_to_existing_skill(self):
        extra = dict(schema_version=1, skill_id='lifetime.objects', theory=[], references=[],
                     theory_bindings=[], reference_bindings=[])
        extra['references'] = [dict(fixture()['references'][0], catalog_order=1, id='followup')]
        self.path.with_name('materials-followup.json').write_text(json.dumps(extra), encoding='utf-8')
        data = self.load(fixture())
        self.assertEqual([item['id'] for item in data['references']], ['sample', 'followup'])
        self.assertEqual(data['owners'], [('lifetime.objects', 'lifetime')])

    def test_catalog_without_specialized_theory_bindings_is_valid_cpp(self):
        item = fixture()
        item['theory_bindings'] = []
        output = self.root / 'empty-generated'
        self.generator.generate(self.load(item), output)
        source = self.root / 'empty.cc'
        source.write_text('#include <array>\n#include "learning/theory_catalog.h"\n'
                          '#include "theory_catalog_data.inc"\nint main() {}\n', encoding='utf-8')
        result = subprocess.run([COMPILER, '-std=c++23', '-pedantic-errors', '-fsyntax-only',
                                 '-I' + str(ROOT / 'src'), '-I' + str(output), str(source)],
                                capture_output=True, text=True, check=False)
        self.assertEqual(result.returncode, 0, result.stderr)

    def test_invalid_authoring_fails_closed(self):
        cases = []
        for field, value in [('schema_version', 2), ('schema_version', True), ('skill_id', 'unknown'),
                             ('extra', 'ignored'), ('references', None)]:
            item = fixture()
            item[field] = value
            cases.append(item)
        for section, field, value in [
            ('theory', 'primary', 'true'), ('theory', 'catalog_order', True),
            ('theory', 'catalog_order', 1), ('theory', 'id', ''),
            ('theory', 'example', '\0'), ('theory', 'source_kind', 'other'),
            ('theory', 'sources', []), ('theory', 'sources', ['https://example.com']),
            ('references', 'source_kind', 'other'), ('references', 'sources', ['https://eel.is/c++draft/basic.life']),
            ('theory_bindings', 'theory_id', 'unknown'), ('theory_bindings', 'question_family_id', ''),
            ('theory_bindings', 'question_family_id', 'unknown_family'),
            ('reference_bindings', 'question_id', 'unknown'),
            ('reference_bindings', 'content_revision', 'b' * 64),
            ('reference_bindings', 'content_revision', 'A' * 64),
            ('reference_bindings', 'reference_ids', ['unknown']),
            ('reference_bindings', 'reference_ids', ['sample', 'sample']),
            ('reference_bindings', 'reference_ids', []),
        ]:
            item = fixture()
            item[section][0][field] = value
            cases.append(item)
        for section in ('theory', 'references', 'theory_bindings', 'reference_bindings'):
            item = fixture()
            item[section].append(deepcopy(item[section][0]))
            cases.append(item)
            item = fixture()
            item[section][0]['unknown'] = 'must not enter snapshots'
            cases.append(item)
        for item in cases:
            with self.subTest(item=item), self.assertRaises(ValueError):
                self.load(item)

    def test_duplicate_json_keys_and_misplaced_files_are_rejected(self):
        self.path.write_text('{"schema_version":1,"schema_version":1}', encoding='utf-8')
        with self.assertRaises(ValueError):
            self.generator.load_materials(self.root, self.skills, self.questions)
        self.path.write_text(json.dumps(fixture()), encoding='utf-8')
        self.path.rename(self.path.with_name('unlisted.json'))
        with self.assertRaises(ValueError):
            self.generator.load_materials(self.root, self.skills, self.questions)

    def test_cli_rejects_malformed_card_identity_without_writing_outputs(self):
        self.load(fixture())
        questions = self.root / 'questions'
        questions.mkdir()
        (questions / 'bad.json').write_text('{"id":[],"skill_id":"lifetime.objects"}', encoding='utf-8')
        output = self.root / 'invalid-output'
        result = subprocess.run([sys.executable, str(SCRIPT), '--content-root', str(self.root),
                                 '--questions', str(questions), '--output-dir', str(output)],
                                capture_output=True, text=True, check=False)
        self.assertNotEqual(result.returncode, 0)
        self.assertNotIn('Traceback', result.stderr)
        self.assertFalse(output.exists())


if __name__ == '__main__':
    unittest.main()
