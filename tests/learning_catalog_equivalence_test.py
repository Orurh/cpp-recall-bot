"""Check real generated C++ data against authoring JSON and frozen legacy bytes."""
import importlib.util
import json
from pathlib import Path
import subprocess
import sys
import unittest


ROOT = Path(__file__).resolve().parents[1]
EXECUTABLE = Path(sys.argv.pop(1)).resolve()
spec = importlib.util.spec_from_file_location('learning_generator', ROOT / 'scripts/generate_learning_content.py')
generator = importlib.util.module_from_spec(spec)
spec.loader.exec_module(generator)

# Explicit separately reviewed CONTENT-02 changes; the migration fixtures above
# remain immutable. No other legacy card revision or help selection is exempt.
REVIEWED_CARD_REVISIONS = {
    'stable_sort_equivalent_order': '0e72214fbf2c347db9fcb2602c4308658b32638080016e1e56cd0df140023e50',
}
REVIEWED_HELP_SELECTIONS = {
    # MOVE-01: reviewed new primary theory for the existing move fundamentals skill.
    # No historical question content/answer revision changes.
    'const_move_001': {
        'id': 'const_move_001', 'references': [], 'theory': 'move_semantics.cast_and_resource_transfer',
    },
    'move_semantics_definition': {
        'id': 'move_semantics_definition', 'references': [],
        'theory': 'move_semantics.cast_and_resource_transfer',
    },
    # These three unchanged cards reuse families newly covered by CONTENT-02.
    # Their question SHA and ordered references remain pinned to the legacy
    # fixtures below; CONTENT-02 added theory and the separately dated
    # preparation supplement adds exact before-answer articles without edits.
    'map_node_handle_change_key': {
        'id': 'map_node_handle_change_key',
        'references': ['content02.preparation.node_access'],
        'theory': 'content02.node_ownership',
    },
    'ranges_find_rvalue_vector_result': {
        'id': 'ranges_find_rvalue_vector_result',
        'references': ['content02.preparation.borrowed_prerequisite'],
        'theory': 'content02.borrowed_owner_boundary',
    },
    'span_after_vector_reallocation': {
        'id': 'span_after_vector_reallocation',
        'references': ['vector_iterators', 'span'],
        'theory': 'content02.view_live_extent',
    },
    'stable_sort_equivalent_order': {
        'id': 'stable_sort_equivalent_order',
        'references': ['content02.stable_ordering.reference'],
        'theory': 'content02.stable_ordering',
    },
    'flat_map_min_dangling_reference': {
        'id': 'flat_map_min_dangling_reference',
        'references': ['content02.preparation.flat_map_storage'], 'theory': None,
    },
    'map_transparent_lookup': {
        'id': 'map_transparent_lookup',
        'references': ['content02.preparation.lookup_comparison'], 'theory': None,
    },
    'optional_value_or_eager_argument': {
        'id': 'optional_value_or_eager_argument',
        'references': ['content02.preparation.argument_or_action'], 'theory': None,
    },
    'unordered_map_iterator_member_address_completion': {
        'id': 'unordered_map_iterator_member_address_completion',
        'references': ['content02.preparation.iterator_pair_access'], 'theory': None,
    },
    'unordered_map_reserve_vs_rehash': {
        'id': 'unordered_map_reserve_vs_rehash',
        'references': ['content02.preparation.hash_table_units'], 'theory': None,
    },
    'vector_reserve_bug_line': {
        'id': 'vector_reserve_bug_line',
        'references': ['content02.preparation.vector_live_elements'], 'theory': None,
    },
}


class CompiledCatalogTest(unittest.TestCase):
    def test_legacy_bytes_and_all_authoring_records_match_compiled_catalog(self):
        fixture = ROOT / 'tests/fixtures/learning_catalog_legacy.json'
        material_fixture = ROOT / 'tests/fixtures/learning_materials_legacy.json'
        result = subprocess.run([str(EXECUTABLE), str(material_fixture)], cwd=ROOT,
                                check=False, capture_output=True, text=True)
        self.assertEqual(result.returncode, 0, result.stderr)
        actual = json.loads(subprocess.check_output([str(EXECUTABLE), '--dump'], cwd=ROOT))
        questions = {item['id']: dict(json.loads(item['snapshot']), content_revision=item['revision'])
                     for item in actual['questions']}
        authoring = generator.load_materials(ROOT / 'content/cpp', questions=questions)
        for section in generator.SECTIONS:
            expected = []
            for item in authoring[section]:
                record = {key: value for key, value in item.items() if key != 'catalog_order'}
                if section in ('references', 'reference_bindings'):
                    record.pop('skill_id')
                expected.append(record)
            compiled = [{key: value for key, value in item.items() if key not in ('snapshot', 'revision')}
                        for item in actual[section]]
            self.assertEqual(compiled, expected, section + ' diverged between authoring and C++')
        legacy = json.loads(fixture.read_text(encoding='utf-8'))
        frozen = json.loads(material_fixture.read_text(encoding='utf-8'))
        self.assertEqual(frozen['legacy_aggregate_sha256'], legacy['catalog_sha256'])
        self.assertEqual(legacy['reference_revisions'],
                         [{'id': item['id'], 'revision': item['revision']}
                          for item in actual['references'][:legacy['counts']['references']]])
        cards = {item['id']: item for item in actual['questions']}
        selections = {item['id']: item for item in actual['selections']}
        for expected in frozen['questions']:
            self.assertIn(expected['id'], cards)
            revision = REVIEWED_CARD_REVISIONS.get(expected['id'], expected['revision'])
            self.assertEqual(cards[expected['id']]['revision'], revision, expected['id'])
        for expected in frozen['selections']:
            reviewed = REVIEWED_HELP_SELECTIONS.get(expected['id'], expected)
            self.assertEqual(selections[expected['id']], reviewed, expected['id'])


if __name__ == '__main__':
    unittest.main()
