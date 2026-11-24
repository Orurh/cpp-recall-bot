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
    # CONTENT-QA 02: exact revised map cards.
    'map_find_iterator_completion': '46204af19d3aad40cc0bc7e9bdcad2581ef5a70e3d612b85ca89d6bb2e8b6247',
    'map_contains_completion': '712b86b51680cc979951483fc699e67e2f3bee00f7dd0d533eb07f1934ac4586',
    'map_try_emplace_completion': 'cb92c46529e22f3c423a2d5a1e92cda8c5f19c54a6ab42b88066b4b6a51d4c28',
    'map_insert_or_assign_completion': '22697b13e1d3c669bb013c262f564d71ab4181fd73623f5e1e155cc18c76b9bd',
    'map_find_without_insertion': '86145cfba2b15d294d7405084d67fa86f6828b687cd2e2030aafb67191fee2ab',
    'map_insert_or_assign_vs_try_emplace': 'ed2bd20124d1c385c51bec61a63ecc7d547e7dad5b05bb09b832be93a5de0b80',
    'review_cache_read_contract': '2b333c51c95d958766e98b663f472af9d66c8febff9a9fae9803c3afa3a15956',
    'stable_sort_equivalent_order': '0e72214fbf2c347db9fcb2602c4308658b32638080016e1e56cd0df140023e50',
    # Reviewed quality fixes: options changed, answer keys and families retained.
    'weak_ptr_lock_is_atomic_access': '019de58b089d2321bb925711053e10f6f034f136ec138aafca719138b296957c',
    'ranges_search_local_owner_return': 'cbcf066f15282c4cd071ccacb277557e14d43612b6a2d76ac7aa6badddcc9a48',
}
REVIEWED_HELP_SELECTIONS = {
    # CONTENT-QA 02: narrow help before and after an error.
    'map_find_iterator_completion': {
        'id': 'map_find_iterator_completion', 'references': ['qa20261124.map_lookup_checklist'],
        'theory': 'qa20261124.map_contract_checklist',
    },
    'map_contains_completion': {
        'id': 'map_contains_completion', 'references': ['qa20261124.map_lookup_checklist'],
        'theory': 'qa20261124.map_contract_checklist',
    },
    'map_try_emplace_completion': {
        'id': 'map_try_emplace_completion', 'references': ['qa20261124.map_mutation_checklist'],
        'theory': 'qa20261124.map_contract_checklist',
    },
    'map_insert_or_assign_completion': {
        'id': 'map_insert_or_assign_completion', 'references': ['qa20261124.map_mutation_checklist'],
        'theory': 'qa20261124.map_contract_checklist',
    },
    'review_cache_read_contract': {
        'id': 'review_cache_read_contract', 'references': ['qa20261124.map_lookup_checklist'],
        'theory': 'qa20261124.map_contract_checklist',
    },
    'review_mark_existing_contract': {
        'id': 'review_mark_existing_contract', 'references': ['qa20261124.map_mutation_checklist'],
        'theory': 'qa20261124.map_contract_checklist',
    },
    'map_lookup_methods_matching': {
        'id': 'map_lookup_methods_matching', 'references': ['qa20261124.map_lookup_checklist'],
        'theory': 'qa20261124.map_contract_checklist',
    },
    'map_subscript_inserts_default_value': {
        'id': 'map_subscript_inserts_default_value', 'references': ['qa20261124.map_lookup_checklist'],
        'theory': 'qa20261124.map_contract_checklist',
    },
    'map_find_without_insertion': {
        'id': 'map_find_without_insertion', 'references': ['qa20261124.map_lookup_checklist'],
        'theory': 'qa20261124.map_contract_checklist',
    },
    'map_insert_or_assign_vs_try_emplace': {
        'id': 'map_insert_or_assign_vs_try_emplace', 'references': ['qa20261124.map_mutation_checklist'],
        'theory': 'qa20261124.map_contract_checklist',
    },
    'map_update_existing_stock': {
        'id': 'map_update_existing_stock', 'references': ['qa20261124.map_mutation_checklist'],
        'theory': 'qa20261124.map_contract_checklist',
    },
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
        'references': ['qa20261124.map_probe_contract'], 'theory': None,
    },
    'ranges_sort_list_constraints': {
        'id': 'ranges_sort_list_constraints',
        'references': ['qa20261124.algorithm_constraints'], 'theory': None,
    },
    'vector_raii_early_return': {
        'id': 'vector_raii_early_return',
        'references': ['qa20261124.automatic_scope_check'], 'theory': 'raii.scope_cleanup',
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
