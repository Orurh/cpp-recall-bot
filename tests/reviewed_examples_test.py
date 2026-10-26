"""Reproduce only reviewed, well-defined examples; execution does not prove C++ rules."""
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[1]
COMPILER = sys.argv.pop(1) if len(sys.argv) > 1 else 'c++'


class ReviewedExamplesTest(unittest.TestCase):
    def test_projected_key_alias_forms(self):
        card = json.loads((ROOT / 'content/questions/templates/sorted_snapshot_key_alias_completion.json').read_text())
        fixtures = '''
#include <string>
struct Record { std::string name; };
struct KeyOf {
  const std::string& operator()(const Record&) const &;
  int operator()(const Record&) &;
  double operator()(const Record&) &&;
  long operator()(Record&) const &;
};
struct PointerKey { const int* const& operator()(const Record&) const; };
static_assert(std::is_same_v<SortedSnapshot<Record, KeyOf>::Key, std::string>);
static_assert(std::is_same_v<SortedSnapshot<Record, PointerKey>::Key, const int*>);
static_assert(std::is_same_v<SortedSnapshot<Record, decltype(&Record::name)>::Key, std::string>);
'''
        with tempfile.TemporaryDirectory(prefix='cpp-recall-key-alias-') as directory:
            source = Path(directory) / 'alias.cc'
            for answer in card['task']['accepted_answers']:
                with self.subTest(answer=answer):
                    declaration = answer if answer.startswith('using ') else 'using Key = ' + answer.rstrip(';') + ';'
                    code = card['code'].replace('using Key = /* допиши тип */;', declaration)
                    source.write_text(code + '\n' + fixtures)
                    result = subprocess.run([COMPILER, '-std=c++23', '-Wall', '-Wextra', '-pedantic-errors',
                                             '-fsyntax-only', str(source)], capture_output=True, text=True,
                                            timeout=40, check=False)
                    self.assertEqual(result.returncode, 0, result.stderr)

    def test_actual_card_programs(self):
        # Expected results are independently recorded by the source review, not read from the answer key.
        cases = {
            'atomic_fetch_add_returns_old_value': '57',
            'compare_exchange_updates_expected': '022',
            'deferred_future_wait_for_status': '1',
            'shared_future_deferred_side_effect': '77',
            'shared_future_repeated_get': '77',
            'placement_new_starts_new_lifetime': '2',
            'invoke_member_data_write': '9',
            'invoke_const_member_function': '10',
            'lower_bound_first_equal_position': '1',
            'lower_bound_descending_boundary': '60',
            'span_observes_owner_changes': '18',
            'const_span_mutable_elements': '17',
            'comparator_equivalence_not_equality': '1',
            'ranges_sort_member_projection': 'abc',
            'parameter_pack_element_count': '3:3',
            'parameter_pack_expand_call': '7',
            'fold_and_nonempty_pack': '10',
            'fold_subtraction_grouping': '12 16',
            'fold_and_empty_pack_identity': '110',
            'binary_fold_empty_pack_initial_value': '06',
            'vector_resize_adds_elements': '4 7',
            'find_missing_end_guard': 'нет',
            'vector_erase_return_next': '9 3',
            'vector_erase_filter_loop': '1 5 ',
            'map_find_without_insertion': '1 0 1',
            'map_insert_or_assign_vs_try_emplace': '4 1',
            'map_update_existing_stock': '3 1',
        }
        bank = {q['id']: q for p in (ROOT / 'content/questions').rglob('*.json')
                for q in [json.loads(p.read_text(encoding='utf-8'))]}
        with tempfile.TemporaryDirectory(prefix='cpp-recall-reviewed-examples-') as directory:
            for question_id, expected in cases.items():
                with self.subTest(question_id=question_id):
                    source = Path(directory) / f'{question_id}.cc'
                    binary = Path(directory) / question_id
                    source.write_text(bank[question_id]['code'], encoding='utf-8')
                    compiled = subprocess.run(
                        [COMPILER, '-std=c++23', '-pthread', '-Wall', '-Wextra', '-pedantic-errors',
                         str(source), '-o', str(binary)], capture_output=True, text=True, timeout=40,
                        check=False)
                    self.assertEqual(compiled.returncode, 0, compiled.stderr)
                    executed = subprocess.run([str(binary)], capture_output=True, text=True,
                                              timeout=5, check=False)
                    self.assertEqual(executed.returncode, 0, executed.stderr)
                    self.assertEqual(executed.stdout, expected)


if __name__ == '__main__':
    unittest.main()
