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
    'map_node_handle_change_key': 'a55f6f3164e994f7fab6618d312a0d9df70a44b911a012757d79d134700c0b8d',
    'flat_map_min_dangling_reference': '7ebf00353e7927db5edf9f4a84ad7576e711e39f6d2dc1a8f3797b53f4689cd0',
    'optional_value_or_eager_argument': '1ecad3ead84cba0ba0e6ae744ae5f984cf3d2e016e04663d26c04a9f0b54f2e1',
    'const_span_mutable_elements': '173233365a5482ed3aece5135804e53cc0aee502aaca1324258bc30fd58b1d23',
    'ranges_find_rvalue_vector_result': 'ba00339c2ba358ba766695996df60d095a8a30ebe819832a2cf7d4a45c5e4365',
    'ranges_sort_list_constraints': '3dd0a49c6089b3a01a5dc204896f175f9c34f6c4e96522891f3183d29dd81fe0',
    'span_after_vector_reallocation': '09577dfc92198d580aa11b13b3009a002823c4aa10dbf9deffa54e78a71ca818',
    'string_view_from_temporary_string': 'b4197423bbd015fb83bf836d2311204f41059e1d4aff6aed469106234d59bfcf',
    'lower_bound_partition_precondition': '08127876b1c915a6693d344ca260cb5a39f113d8e9d7642230b6f67ecb72b72f',
    'lower_bound_descending_boundary': '1d844ffca6dd9f9ca73c3cec730ba8472f7e0d63be9a9f448f6b886aebb217ed',
    'lower_bound_candidate_not_match': '3ddbef9422d8b33852d1f66f9c528bf4237e182799cd73fc52bfb1840d3e1cb9',
    'comparator_equivalence_not_equality': '12edec0f7db60cdc8a55504726b00f6f05ea86dd9ef3dce2188a61e0aaa06219',
    'ranges_sort_member_projection': 'acc97545fe432e8008741e2bdd8dc208f7544ec10f164abd4173c322e7235d1d',
    # CONTENT-QA batch 01: exact successor revisions; frozen fixture stays unchanged.
    'erase_remove_vector_size': '3e0330a315904527f41ac177df5375a63f430f01c12006a9809cd8f14c6d32d3',
    'vector_access_complexity_matching': '23608c1c712cc6738a2ed4b80bcecc7aefd44154ab1efcc8a4e0fcadf898076b',
    'vector_invalidation_multiple_choice': 'c63ebc79eede7ed587f1f2328ae3889f9ec1969b443fe0a349b53b9e182e45ac',
    'vector_erase_filter_loop': '85e46436729bcc5413077f28849ddc5c8929a923244718f9fca7e27953a2765b',
    'vector_reserve_bug_line': '9dca3e43cc99c9970435d174510f3d75ffec5aa4019e61751261f28d23e7af16',
    # CONTENT-QA 03: exact revised unordered-map cards.
    'unordered_map_iterator_member_address_completion': 'be2a8d4f95b466a9e066f6629dcab1a9640b51e3f0219c0bbbcec44319265937',
    'unordered_map_reserve_vs_rehash': '2dcbf46f253b7c86224e8024134d3562a5298cfc2cf7c88ab84ca68658d8f3dc',
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
    'ranges_search_local_owner_return': '295306949c8a1340a2fafa59c9f285824fb24828f0b1a9b14c2bc024c3263a3b',
}
REVIEWED_HELP_SELECTIONS = {
    'invoke_const_member_function': {
        'id': 'invoke_const_member_function', 'references': ['qa20261125.invoke_method_trace'], 'theory': None,
    },
    'invoke_member_data_write': {
        'id': 'invoke_member_data_write', 'references': ['qa20261125.invoke_member_trace'], 'theory': None,
    },
    'const_span_mutable_elements': {
        'id': 'const_span_mutable_elements', 'references': ['qa20261125.span_const_trace'],
        'theory': None,
    },
    'span_observes_owner_changes': {
        'id': 'span_observes_owner_changes', 'references': ['qa20261125.span_alias_trace'],
        'theory': None,
    },
    'string_view_from_temporary_string': {
        'id': 'string_view_from_temporary_string', 'references': ['qa20261125.string_owner_trace'],
        'theory': 'lifetime.references',
    },
    'lower_bound_partition_precondition': {
        'id': 'lower_bound_partition_precondition', 'references': ['qa20261125.boundary_trace'], 'theory': None,
    },
    'lower_bound_first_equal_position': {
        'id': 'lower_bound_first_equal_position', 'references': ['qa20261125.boundary_trace'], 'theory': None,
    },
    'lower_bound_descending_boundary': {
        'id': 'lower_bound_descending_boundary', 'references': ['qa20261125.boundary_trace'], 'theory': None,
    },
    'lower_bound_candidate_not_match': {
        'id': 'lower_bound_candidate_not_match', 'references': ['qa20261125.boundary_trace'], 'theory': None,
    },
    'comparator_equivalence_not_equality': {
        'id': 'comparator_equivalence_not_equality', 'references': ['qa20261125.order_pair_trace'], 'theory': None,
    },
    'ranges_sort_member_projection': {
        'id': 'ranges_sort_member_projection', 'references': ['qa20261125.projection_trace'], 'theory': None,
    },
    'stl_algorithms_matching': {
        'id': 'stl_algorithms_matching', 'references': ['qa20261125.algorithm_contract_check'], 'theory': None,
    },
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
        'references': ['qa20261125.node_detached_trace'],
        'theory': 'qa20261125.node_ownership_principle',
    },
    'ranges_find_rvalue_vector_result': {
        'id': 'ranges_find_rvalue_vector_result',
        'references': ['qa20261125.borrowed_result_trace'],
        'theory': 'qa20261125.range_owner_principle',
    },
    'span_after_vector_reallocation': {
        'id': 'span_after_vector_reallocation',
        'references': ['qa20261125.span_address_trace'],
        'theory': 'qa20261125.view_validity_principle',
    },
    'stable_sort_equivalent_order': {
        'id': 'stable_sort_equivalent_order',
        'references': ['qa20261125.stable_order_trace'],
        'theory': 'qa20261125.stable_order_principle',
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
        'references': ['qa20261124.unordered_member_path'], 'theory': None,
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
