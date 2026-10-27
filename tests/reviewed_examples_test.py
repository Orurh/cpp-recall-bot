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
    def test_reviewed_reference_examples(self):
        # The C++ matrix test verifies these exact examples against the runtime
        # catalog. Only known, well-defined snippets are executed here.
        ledger = json.loads((ROOT / 'docs/reviews/references/2026-10-26-bindings.json').read_text())
        examples = {record['id']: record['example'] for record in ledger['references']}
        checks = {
            'object_slicing': 'assert(copy.corners() == 0); assert(alias.corners() == 3);',
            'virtual_dispatch': 'assert(device.code() == 9);',
            'virtual_destruction': 'static_assert(std::has_virtual_destructor_v<Resource>); '
                                   'assert(owner); owner.reset(); assert(!owner);',
        }
        with tempfile.TemporaryDirectory(prefix='cpp-recall-reference-examples-') as directory:
            for reference_id, check in checks.items():
                with self.subTest(reference_id=reference_id):
                    example = examples[reference_id]
                    # Declarations belong at namespace scope: in particular the
                    # out-of-class pure virtual destructor definition.
                    boundary = {'object_slicing': 'Triangle original;',
                                'virtual_dispatch': 'Sensor sensor;',
                                'virtual_destruction': 'std::unique_ptr<Resource> owner'}[reference_id]
                    declarations, statements = example.split(boundary, 1)
                    code = ('#include <cassert>\n#include <memory>\n#include <type_traits>\n' + declarations
                            + '\nint main() {\n' + boundary + statements + '\n' + check + '\n}\n')
                    source = Path(directory) / 'reference.cc'
                    binary = Path(directory) / 'reference'
                    source.write_text(code, encoding='utf-8')
                    compiled = subprocess.run(
                        [COMPILER, '-std=c++23', '-Wall', '-Wextra', '-pedantic-errors', str(source), '-o', str(binary)],
                        capture_output=True, text=True, timeout=40, check=False)
                    self.assertEqual(compiled.returncode, 0, compiled.stderr)
                    executed = subprocess.run([str(binary)], capture_output=True, text=True, timeout=5, check=False)
                    self.assertEqual(executed.returncode, 0, executed.stderr)

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

    def test_concrete_projected_key_types(self):
        cases = {
            'projected_name_key_concrete_type': 'std::string',
            'projected_pointer_key_concrete_type': 'const int*',
        }
        with tempfile.TemporaryDirectory(prefix='cpp-recall-concrete-key-') as directory:
            source = Path(directory) / 'key.cc'
            for question_id, expected in cases.items():
                card = json.loads((ROOT / f'content/questions/templates/{question_id}.json').read_text())
                with self.subTest(question_id=question_id):
                    code = card['code'] + '\nstatic_assert(std::is_same_v<Key, ' + expected + '>);\n'
                    # Check both the displayed alias and every accepted concrete spelling.
                    code += '\n'.join('static_assert(std::is_same_v<' + answer.rstrip(';') + ', Key>);'
                                      for answer in card['task']['accepted_answers'])
                    source.write_text(code, encoding='utf-8')
                    result = subprocess.run([COMPILER, '-std=c++23', '-Wall', '-Wextra', '-pedantic-errors',
                                             '-fsyntax-only', str(source)], capture_output=True, text=True,
                                            timeout=40, check=False)
                    self.assertEqual(result.returncode, 0, result.stderr)

    def test_template_implementation_forms(self):
        cases = {
            'fold_validate_each_measurement': (
                'assert(readings_ok(0)); assert(readings_ok(0, 100, 45)); '
                'assert(!readings_ok(-1)); assert(!readings_ok(20, 101, 45)); '
                'assert(!readings_ok(80, -2));'),
            'binary_fold_registration_fee': (
                'assert(registration_fee(25) == 25); assert(registration_fee(9, 6) == 15); '
                'assert(registration_fee(25, 3, 4) == 32); assert(registration_fee(0, 2, 0, 5) == 7);'),
        }
        with tempfile.TemporaryDirectory(prefix='cpp-recall-template-implementation-') as directory:
            source = Path(directory) / 'fold.cc'
            binary = Path(directory) / 'fold'
            for question_id, checks in cases.items():
                card = json.loads((ROOT / f'content/questions/templates/{question_id}.json').read_text())
                for answer in card['task']['accepted_answers']:
                    with self.subTest(question_id=question_id, answer=answer):
                        statement = answer if answer.startswith('return ') else 'return ' + answer.rstrip(';') + ';'
                        code = card['code'].replace('return /* допиши выражение */;', statement)
                        source.write_text('#include <cassert>\n' + code + '\nint main() { ' + checks + ' }\n',
                                          encoding='utf-8')
                        compiled = subprocess.run(
                            [COMPILER, '-std=c++23', '-Wall', '-Wextra', '-pedantic-errors', str(source), '-o', str(binary)],
                            capture_output=True, text=True, timeout=40, check=False)
                        self.assertEqual(compiled.returncode, 0, compiled.stderr)
                        executed = subprocess.run([str(binary)], capture_output=True, text=True, timeout=5, check=False)
                        self.assertEqual(executed.returncode, 0, executed.stderr)

    def test_lifetime_reference_examples(self):
        ledger = json.loads((ROOT / 'docs/reviews/references/2026-10-26-bindings.json').read_text())
        examples = {record['id']: record['example'] for record in ledger['references']}
        checks = {
            'unique_ptr_access': 'assert(*owner == 25); assert(*view == 25); assert(owner.get() == view);',
            'unique_ptr_move': 'assert(!first); assert(second.get() == view); assert(*view == 18);',
            'unique_ptr_reset': 'assert(owner); assert(*owner == 25);',
        }
        with tempfile.TemporaryDirectory(prefix='cpp-recall-raii-references-') as directory:
            for reference_id, check in checks.items():
                with self.subTest(reference_id=reference_id):
                    source = Path(directory) / 'reference.cc'
                    binary = Path(directory) / 'reference'
                    source.write_text('#include <cassert>\n#include <memory>\n#include <utility>\n'
                                      'int main() {\n' + examples[reference_id] + '\n' + check + '\n}\n',
                                      encoding='utf-8')
                    compiled = subprocess.run(
                        [COMPILER, '-std=c++23', '-Wall', '-Wextra', '-pedantic-errors', str(source), '-o', str(binary)],
                        capture_output=True, text=True, timeout=40, check=False)
                    self.assertEqual(compiled.returncode, 0, compiled.stderr)
                    executed = subprocess.run([str(binary)], capture_output=True, text=True, timeout=5, check=False)
                    self.assertEqual(executed.returncode, 0, executed.stderr)

    def test_lifetime_theory_examples(self):
        # core_tests verifies that these fixtures are the exact examples served
        # by TheoryCatalog. Neither example executes dangling-pointer access.
        review = json.loads((ROOT / 'docs/reviews/theory/2026-10-27-lifetime-examples.json').read_text())
        self.assertEqual(review['standard_document'], 'N4950')
        self.assertEqual(len(review['reviews']), 2)
        with tempfile.TemporaryDirectory(prefix='cpp-recall-theory-examples-') as directory:
            for record in review['reviews']:
                with self.subTest(theory=record['id']):
                    source = Path(directory) / 'example.cc'
                    binary = Path(directory) / 'example'
                    source.write_text('#include <cassert>\n' + record['example']
                                      + '\nint main() {\n' + record['assertions'] + '\n}\n', encoding='utf-8')
                    compiled = subprocess.run(
                        [COMPILER, '-std=c++23', '-Wall', '-Wextra', '-pedantic-errors', str(source), '-o', str(binary)],
                        capture_output=True, text=True, timeout=40, check=False)
                    self.assertEqual(compiled.returncode, 0, compiled.stderr)
                    executed = subprocess.run([str(binary)], capture_output=True, text=True, timeout=5, check=False)
                    self.assertEqual(executed.returncode, 0, executed.stderr)

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
            'parameter_pack_frame_arguments': '4279',
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
            'block_scope_destruction_order': 'BCA',
            'pointer_reference_share_object': '9 9',
            'unique_ptr_make_get_observe': '8 1',
            'unique_ptr_reset_destroys_now': 'AD1',
            'raii_unique_ptr_caught_exception': 'ARC',
            # The displayed program is defined: the question asks about a
            # hypothetical post-reset read. Never add that invalid read here.
            'unique_ptr_borrow_across_owner_move': '',
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
