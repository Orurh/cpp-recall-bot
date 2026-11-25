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
    # QA15–17: exact reviewed successor revisions.
    'decltype_auto_parenthesized_return': '2abf07569b335530e61e7124eb8b4453bd20a8f86c9a60c50afbd7c8574d16e1',
    'template_by_value_drops_top_const': '428ba3dac581257a8fb740a9c1c3164ec7c2ceb8fea0f85ad559beabfdbb2d35',
    'dependent_member_template_keyword': '1958fa9b5808a68dbd332e0726abd3215d9d28f407913faaf5f9960ede32ec7d',
    'dependent_type_requires_typename': '6c71b2f660e5d4638fde8542cef4addf8d15506533170a9c4cd9944a677914ae',
    'function_template_partial_specialization': 'b4701944b71344b27f3934107876c43831a631a77ccd27a7e538001ed930853e',
    'templates_implicit_instantiation_pointer': '013fd00c20ce22a05c1fd8e8c2d51a6013919d8db907a655a4afef7aa1c4a441',
    'unused_member_not_instantiated': '0a3af5b0d62c014e30e9773b66fd5595e59d3bd43ab827b6f8ed5b6a22456bfe',
    'if_constexpr_non_dependent_invalid_branch': '2a272e30400fef81fb800df8b0df2501fda9dfa13a4b9357e1123a89598efec5',
    'requires_expression_invalid_requirement': 'b4561e0c329f59de0a8aa12b6d5833a7f570aa12ca94a7aa9335f997d62f8f7b',
    'parameter_pack_element_count': 'f43aad82b951be877433d73b801d1cd6657c096a729fbdd86ce102ae7ba62d28',
    'parameter_pack_expand_call': '093bc85ed80e60e9431542087d6a1d48655907bdd09180b493862ae5d85f07f5',
    'fold_and_empty_pack_identity': '18cdc85e1923d8152276b7412440b2a25350d408802ad045346fa2977f6e9541',
    'binary_fold_empty_pack_initial_value': '302c908f32f592e5c8d729e788311bc0f7ba406582125598018ee3c69b6e2dad',
    'atomic_fetch_add_returns_old_value': 'b60b43182b2da8308db0602bf62d33a5c44d654972ccca391baefc1b56235b8d',
    'atomic_flag_lock_free_guarantee': '75c463046daa31b1ed1ce01d915880dd0d428042363b6972967e31a6861f4f88',
    'atomic_relaxed_guarantees': 'cda97be955c0cf7e19bf2c1f430c02eeb5361cf83dd20eb8b38c1a61738e3349',
    'bit_fields_memory_location_data_race': '1a1e6663b3ed12f96e78ef8dd6c8f25bb7ede2b6018955e6660d4aedf7acb5f8',
    'deferred_future_wait_for_status': 'eb9067baff760a85ce1b0f91d7c050a48adda0809501f668f4b3ae709c73fd90',
    'future_get_consumes_state': '1d688a150a0468788ffa24a6157f4edf8b08adf68cc9a844bc76aa5a5f2ae19d',
    'release_acquire_publishes_data': 'e90627222accbabb842caa83b73df7750aa5804e73d902b70c5b5b48153438b9',
    'shared_future_deferred_side_effect': 'ddbe9cb240284e358c70d8c96385b4be052819e718e88baf51c14bea26d77922',
    'shared_future_repeated_get': '4f177cb7cfd276ad5bfb4d01435b9fb02d43202d1467515778f6fc9375cba5d9',
    'comparator_cycle_counterexample': '88d03b716cbeb0c273d90308890cb17c624739242c12adfe0e3136d0977b464e',
    'map_update_existing_stock': '3e3bb4fa0e8f3f83b5d335c4ffe16f6ca0d4fda6d6607bbca9e142655522b1d5',
    # CONTENT-QA 14: exact overload successor revisions.
    'oop_overload_static_dispatch': '9d6d60dc8a5ff0d9872cc073955a7d67ab826eed0583d01c55a11802145f8e7d',
    'oop_overload_then_virtual_dispatch': '8184a542bcbf37b5ee9f81a234360bf000acbfe0e7410c5a7afd819182dda8a1',
    'oop_override_vs_overload': '536f3d7298398a0bb649367ae07f8a8b65b6c95dcf64162078e4cafd75ebb765',
    'oop_private_inheritance_conversion': '6fe165128a2b2eb6205e3027d22edd9ce29a59feae948567b4f8d33930818efa',
    'oop_using_restores_base_overload': 'b62235757cc23708880facd7fbc5f13b57da36207c58df9949ecbb267fa54e74',
    'oop_virtual_default_argument': '040bd48c322372ebd205a786c4b8d735cc9155bd7f58a52cbbf091a658006499',
    'constrained_overload_selection': '87ac430457de838b74cb39fef07635c1f0185b268b1b4184862f88f85f536a3d',
    # CONTENT-QA 13: exact linkage successor revisions.
    'extern_variable_declaration': 'b3588e55b2ad851467d3c42996daad3c150afa25cb9a9f645552d19340850964',
    'inline_keyword_meaning': '4235a846e2551cc725e0393f956a8af72713973281aa5e65165be2e8f880e703',
    'inline_thread_local_across_tus': '04dc0bbbd3f87e39cd3259e779ca6ddaab4001674fbb9738ed374bdf52eeebee',
    'linkage_definition': '7100b7ebe7c384ffc6ea3ebc94f873f8fa9aea1c517daff060cd882bef20d59a',
    'odr_basic_definition': '2708214bc755ca41593ca8243845d17f94d5d698d491202c7eb120abc7e220f0',
    'static_internal_linkage': '9bc54f7d505b2f237b1be7edbb342b680ebfdfe67c680e85f2fa2d39ff2f06ff',
    'translation_unit_definition': '2c3bc432c3649a6184b463e4a5b8d45e01279c74ea30c25905d6447ef27bb2b7',
    # CONTENT-QA 12: exact storage-duration successor revisions.
    'dynamic_object_outlives_pointer_scope': '5d7ebb0ad67579882bb17715387a1328d345706d4b0c6dd0269c1fb524748a2e',
    'language_core_scope_definition': 'feb70e230e13b84bf8ab0d58c6263a15ca2af1d8f37644a3e01a11ee27178930',
    'static_local_persists_between_calls': '1d877d0335de75db85ef02aaa534953ba295b631ed5601a651afe0722b480847',
    'static_object_surrounds_main': '437bc5e221a71fd4ca30635c770e9306af5af51014f1992b59b1ec54993cee57',
    'subobject_inherits_storage_duration': '0cb016ac49c439292c8c663e813868520834da4cc8a966ac2c10b26ad3047f18',
    'thread_local_has_one_object_per_thread': 'bf52e0882aa945f9a7f01b9189913e7f6cb0126e864c48b6a052ed06d6b36006',
    'default_argument_lambda_static': '5e0fb25e9390c4418f19cf1b01607c36a32a0805a42aaba2f3b985565265df48',
    # CONTENT-QA 11: exact reviewed successor revisions.
    'copy_semantics_definition': 'a3ecb0c7d11a54d1f507a0a0a2a3673e904033d8aadfe3a2a26111f2994fa830',
    'move_semantics_definition': '3f534a510efb03db255d85d88ad931de12e2519c241aac8002b1d4697c30b562',
    'copy_constructor_const_ref_non_const_source': 'f11a30d96b4f778ca179eb60c000250053519b514981a0bde575816f87eab6a0',
    'copy_constructor_non_const_ref_const_source': 'b1d9bfd9c0272d0c28b4a94abdd1060d010e3462ffd46d83684fe28e32170b70',
    'special_members_mutable_copy_from_rvalue': '98cdd2ad3d3364b65bd26851eaedc840cf47c95c5ff23e9312d0c61de8ac3584',
    'lvalue_rvalue_basic_definition': '9b21b5cf9f706b8f7417b8d0c09ad4a8acac872723eb303db1680e189a99865a',
    'value_category_name_and_sum': '98c9951b6136a067aeb6d12c98f57f5eac47718a4241bcb7afc874690af1d186',
    'value_category_reference_call_assignment': '74eccadd5efdccd4f32455dfa5298cdd1f5e598529a4a5807d4e62f3a1f1c15c',
    'raii_closes_file_on_early_exit': '068667cf61e8744572df749df17afb5cc151c1c640ce915cfedade23abbd8763',
    'raii_definition': 'ee56b604c3db490d64b36e1d7222b8ce87d5946b6c603be84c1322f6864dfba9',
    'raii_member_destruction_dependency': 'ddbb6bbebbadc64e005cc654bcbe067582752ff689e87130f7c5b0acfe4db2db',
    'raii_move_assignment_releases_old_resource': 'e5b02d5de33763000aac21c1bda7f0d927af6936357b28d88118f059cee302aa',
    'raii_partial_construction_cleanup': '31a74763e8d4303bc7da2e7cb39335647d1133f88f0145ba5328cdec57eb5b0e',
    'raii_raw_owner_copy_double_delete': '65d29273bc89b1200bb5c595ea2206bb77275cc138bba4195ae9f52c7ee8f7a7',
    'raii_reverse_local_destruction_order': '1fc606f43d02b4b10ca11bc04e5bf35f0abd6182e7da9612eb856edf953f1ac4',
    'block_scope_destruction_order': '4cf81e1d2b470d396ac5098466587712c55ce7a52b0a42df75d04266d315ef0a',
    'shared_ptr_alias_keeps_owner_alive': '77884cee0d3591794bcd937e1664ef75720af526018da7fe3aebd57796c711ff',
    'shared_ptr_copy_use_count': 'd3a6e21e4b7c7957c3c1cfd960f06d5ebd4fb535cc14992b66683dbdd8687eef',
    'shared_ptr_cycle_requires_weak_back_reference': '006412688370449d81df4bad1095f4545a7f9cec0513f6817d1c63ac6707987d',
    'shared_ptr_duplicate_control_blocks': 'f1770c1bd4e107ecda6ae5df5b93ac8e32d514046705045c81243e9d02f61025',
    'shared_ptr_same_instance_data_race': 'c461e4bb99267bc7661846a768e933a3fabc1f23adb3698ea874faf58f75df53',
    'unique_ptr_borrow_without_transfer': '637ba463b3f6a51d803001276359d87d85f4e05178aae3680e3760880af46684',
    'unique_ptr_move_transfers_ownership': '9d2bcbb1e14caecbfc4a967ad82f1c4f633d72f8cced6155c59473b76995437d',
    'unique_ptr_scope_deletes_object': '0d212fdc1d089a983fc4a558c1be4a3b60845854cb9057ae446b0f61968de716',
    'aggregate_reference_paren_vs_brace': 'a3d956c7918749aecf2ad74d0db17ae506d52c78449199f33a33c92c364a9056',
    'c_str_from_temporary_dangles': 'd75aed03b588de57bd0b18f8a87e7aeb8f66810dd4346c8b023b14c38fca0e45',
    'const_reference_extends_temporary': '16133dbb52731424eb2697aec180639584ecb0cf5262a30fac38ba3bedd3e98c',
    'optional_emplace_starts_object_lifetime': '88c6c1867b434b6c9766a5c392e41eaac8e1f2e1b501689c0ae008b695b4d4a6',
    'placement_new_starts_new_lifetime': '7ab06572c87dae3ccc639f982e784d6ab485f49fc33ea5ee9c66d88c685d2fa7',
    'pointer_after_local_lifetime': '6718ec7863dd5c9e889e6d843c76489f8d645801c2d82e9908ee0164591e3ca9',
    'return_reference_does_not_extend_temporary': '5d390628d629ebb581077b48d4b0b2eb4bb0c4bdd261d6bb19fd1126d3a2c744',
    'return_rvalue_reference_to_local': 'a2f50e4cefe14f8699bf5217d56bab936f5a680095c56fdbe2491bc9271d761c',
    'const_reference_observes_changes': '6651e1dc5e88323aa0c6de31bb4b4c0413a5d6edd2e1e1e3a2e3b5e413589953',
    'reference_alias_and_copy': 'e3049d669e4308e094a93664dc8a1039936c26539f4e42ef7a1faead7515ba16',
    'reference_definition': '45eb29f7a6a2d722d8c209c3bb945d9981c854fbc3e5ea9706a5d06de25b77e1',
    'reference_member_assignment': 'e681ba182f59738868a413d16b98d8bd588f35626d18124d33548d24b8e84176',
    'reference_parameter_updates_argument': '27d0381cc264275cb7c9ae6feb6d28d353f10c3a91e56c7c92f333a60427a048',
    'reference_return_existing_member': '0811d90fff2e6b9c9ee0f30355a6e5e8833b21da89bc50b0db331721886499be',
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
    'ranges_sort_member_projection': '0e6f222ba8e9aaa4782968cb6e87a36898e5aef89bc29ec42789b005569ce47c',
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
    'sfinae_enable_if_integral_condition': {
        'id': 'sfinae_enable_if_integral_condition',
        'references': ['qa20261125.trait_selection'], 'theory': 'sfinae.substitution_boundary',
    },
    'sfinae_body_error_is_hard_error': {
        'id': 'sfinae_body_error_is_hard_error',
        'references': ['qa20261125.overload_phase_trace'], 'theory': 'sfinae.substitution_boundary',
    },
    'erase_remove_vector_size': {
        'id': 'erase_remove_vector_size', 'references': ['audit20261126.logical_range_trace'], 'theory': None,
    },
    'vector_invalidation_multiple_choice': {
        'id': 'vector_invalidation_multiple_choice', 'references': ['audit20261126.iterator_position_trace'], 'theory': None,
    },
    'vector_reserve_does_not_change_size': {
        'id': 'vector_reserve_does_not_change_size', 'references': ['audit20261126.vector_storage_trace'], 'theory': None,
    },
    'raii_closes_file_on_early_exit': {
        'id': 'raii_closes_file_on_early_exit', 'references': ['qa20261125.file_exit_trace'],
        'theory': 'raii.scope_cleanup',
    },
    'raii_definition': {
        'id': 'raii_definition', 'references': ['qa20261125.raii_concept_trace'],
        'theory': 'qa20261125.raii_lifetime_principle',
    },
    'raii_lock_guard_unlocks_during_unwinding': {
        'id': 'raii_lock_guard_unlocks_during_unwinding', 'references': ['qa20261125.mutex_unwind_trace'],
        'theory': 'qa20261125.raii_lifetime_principle',
    },
    'raii_member_destruction_dependency': {
        'id': 'raii_member_destruction_dependency', 'references': ['qa20261125.member_order_trace'],
        'theory': 'qa20261125.raii_dependency_principle',
    },
    'raii_move_assignment_releases_old_resource': {
        'id': 'raii_move_assignment_releases_old_resource', 'references': ['qa20261125.resource_assignment_trace'],
        'theory': 'qa20261125.raii_special_members_principle',
    },
    'raii_partial_construction_cleanup': {
        'id': 'raii_partial_construction_cleanup', 'references': ['qa20261125.partial_init_trace'],
        'theory': 'qa20261125.raii_dependency_principle',
    },
    'raii_raw_owner_copy_double_delete': {
        'id': 'raii_raw_owner_copy_double_delete', 'references': ['qa20261125.raw_owner_special_members'],
        'theory': 'qa20261125.raii_special_members_principle',
    },
    'raii_reverse_local_destruction_order': {
        'id': 'raii_reverse_local_destruction_order', 'references': ['qa20261125.local_cleanup_trace'],
        'theory': 'qa20261125.raii_dependency_principle',
    },
    'raii_state_rollback_commit': {
        'id': 'raii_state_rollback_commit', 'references': ['scope_cleanup'],
        'theory': 'raii.scope_cleanup',
    },
    'raii_unique_ptr_caught_exception': {
        'id': 'raii_unique_ptr_caught_exception', 'references': ['qa20261125.exception_owner_trace'],
        'theory': 'qa20261125.raii_lifetime_principle',
    },
    'block_scope_destruction_order': {
        'id': 'block_scope_destruction_order', 'references': ['qa20261125.local_cleanup_trace'],
        'theory': 'storage_duration.basics',
    },
    'shared_ptr_alias_keeps_owner_alive': {
        'id': 'shared_ptr_alias_keeps_owner_alive', 'references': ['qa20261125.alias_owner_trace'],
        'theory': 'qa20261125.shared_group_principle',
    },
    'shared_ptr_copy_use_count': {
        'id': 'shared_ptr_copy_use_count', 'references': ['qa20261125.shared_count_trace'],
        'theory': 'qa20261125.shared_group_principle',
    },
    'shared_ptr_cycle_requires_weak_back_reference': {
        'id': 'shared_ptr_cycle_requires_weak_back_reference', 'references': ['qa20261125.ownership_graph_trace'],
        'theory': 'qa20261125.shared_group_principle',
    },
    'shared_ptr_duplicate_control_blocks': {
        'id': 'shared_ptr_duplicate_control_blocks', 'references': ['qa20261125.shared_group_trace'],
        'theory': 'qa20261125.shared_group_principle',
    },
    'shared_ptr_same_instance_data_race': {
        'id': 'shared_ptr_same_instance_data_race', 'references': ['qa20261125.shared_concurrency_trace'],
        'theory': 'qa20261125.concurrent_owner_principle',
    },
    'smart_pointers_matching': {
        'id': 'smart_pointers_matching', 'references': ['smart_pointer_roles'],
        'theory': 'qa20261125.shared_group_principle',
    },
    'unique_ptr_borrow_across_owner_move': {
        'id': 'unique_ptr_borrow_across_owner_move', 'references': ['qa20261125.unique_owner_transition'],
        'theory': 'qa20261125.unique_resource_principle',
    },
    'unique_ptr_borrow_without_transfer': {
        'id': 'unique_ptr_borrow_without_transfer', 'references': ['qa20261125.borrow_api_trace'],
        'theory': 'qa20261125.unique_resource_principle',
    },
    'unique_ptr_make_get_observe': {
        'id': 'unique_ptr_make_get_observe', 'references': ['unique_ptr_access'],
        'theory': 'qa20261125.unique_resource_principle',
    },
    'unique_ptr_move_transfers_ownership': {
        'id': 'unique_ptr_move_transfers_ownership', 'references': ['qa20261125.unique_move_trace'],
        'theory': 'qa20261125.unique_resource_principle',
    },
    'unique_ptr_reset_destroys_now': {
        'id': 'unique_ptr_reset_destroys_now', 'references': ['unique_ptr_reset'],
        'theory': 'qa20261125.unique_resource_principle',
    },
    'unique_ptr_scope_deletes_object': {
        'id': 'unique_ptr_scope_deletes_object', 'references': ['qa20261125.owner_scope_trace'],
        'theory': 'qa20261125.unique_resource_principle',
    },
    'weak_ptr_lock_is_atomic_access': {
        'id': 'weak_ptr_lock_is_atomic_access', 'references': ['qa20261125.weak_lifetime_trace'],
        'theory': 'qa20261125.concurrent_owner_principle',
    },
    'aggregate_reference_paren_vs_brace': {
        'id': 'aggregate_reference_paren_vs_brace', 'references': ['qa20261125.aggregate_initializer_trace'],
        'theory': 'qa20261125.temporary_timeline_principle',
    },
    'c_str_from_temporary_dangles': {
        'id': 'c_str_from_temporary_dangles', 'references': ['qa20261125.cstr_access', 'qa20261125.string_owner_trace'],
        'theory': 'qa20261125.temporary_timeline_principle',
    },
    'const_reference_extends_temporary': {
        'id': 'const_reference_extends_temporary', 'references': ['qa20261125.temporary_binding_trace'],
        'theory': 'qa20261125.temporary_timeline_principle',
    },
    'lifetime_definition': {
        'id': 'lifetime_definition', 'references': [],
        'theory': 'qa20261125.storage_object_principle',
    },
    'optional_emplace_starts_object_lifetime': {
        'id': 'optional_emplace_starts_object_lifetime', 'references': ['qa20261125.object_storage_trace'],
        'theory': 'qa20261125.storage_object_principle',
    },
    'placement_new_starts_new_lifetime': {
        'id': 'placement_new_starts_new_lifetime', 'references': ['qa20261125.object_storage_trace'],
        'theory': 'qa20261125.storage_object_principle',
    },
    'pointer_after_local_lifetime': {
        'id': 'pointer_after_local_lifetime', 'references': ['qa20261125.local_scope_trace'],
        'theory': 'qa20261125.live_access_principle',
    },
    'pointer_reference_share_object': {
        'id': 'pointer_reference_share_object', 'references': ['qa20261125.live_alias_trace'],
        'theory': 'qa20261125.live_access_principle',
    },
    'return_reference_does_not_extend_temporary': {
        'id': 'return_reference_does_not_extend_temporary', 'references': ['qa20261125.returned_owner_trace'],
        'theory': 'qa20261125.temporary_timeline_principle',
    },
    'return_rvalue_reference_to_local': {
        'id': 'return_rvalue_reference_to_local', 'references': ['qa20261125.returned_owner_trace'],
        'theory': 'qa20261125.temporary_timeline_principle',
    },
    'const_reference_observes_changes': {
        'id': 'const_reference_observes_changes', 'references': ['qa20261125.reference_const_trace'],
        'theory': 'qa20261125.reference_access_principle',
    },
    'reference_alias_and_copy': {
        'id': 'reference_alias_and_copy', 'references': ['qa20261125.reference_identity_trace'],
        'theory': 'qa20261125.reference_access_principle',
    },
    'reference_definition': {
        'id': 'reference_definition', 'references': ['qa20261125.reference_identity_trace'],
        'theory': 'qa20261125.reference_access_principle',
    },
    'reference_member_assignment': {
        'id': 'reference_member_assignment', 'references': ['qa20261125.reference_identity_trace'],
        'theory': 'qa20261125.reference_access_principle',
    },
    'reference_parameter_updates_argument': {
        'id': 'reference_parameter_updates_argument', 'references': ['qa20261125.reference_parameter_trace'],
        'theory': 'qa20261125.reference_access_principle',
    },
    'reference_return_existing_member': {
        'id': 'reference_return_existing_member', 'references': ['qa20261125.reference_result_trace'],
        'theory': 'qa20261125.returned_access_principle',
    },
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
        'theory': 'qa20261125.temporary_timeline_principle',
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
        'references': ['audit20261126.index_precondition_trace'], 'theory': None,
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
