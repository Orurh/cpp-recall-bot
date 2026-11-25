"""Reproduce only reviewed, well-defined examples; execution does not prove C++ rules."""
import hashlib
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[1]
COMPILER = sys.argv.pop(1) if len(sys.argv) > 1 else 'c++'


def theory_records():
    # Compiled equivalence tests independently check these authoring strings
    # against the actual generated C++ catalog and immutable legacy digest.
    result = {}
    for path in (ROOT / 'content/cpp/topics').glob('*/skills/*/materials*.json'):
        material = json.loads(path.read_text(encoding='utf-8'))
        for block in material['theory']:
            record = {key: value for key, value in block.items()
                      if key not in ('catalog_order', 'source_kind')}
            record['skill_id'] = material['skill_id']
            if record['id'] in result:
                raise ValueError('duplicate theory id: ' + record['id'])
            result[record['id']] = record
    return result


class ReviewedExamplesTest(unittest.TestCase):
    def test_container_preparation_examples_compile(self):
        # Compile the delivered authoring examples, not rewritten companions.
        # Exact compiled-catalog equivalence and the dated review separately pin
        # the texts. Syntax acceptance is not proof of a library guarantee.
        examples = self.container_preparation_examples()
        ordinary = {key: value for key, value in examples.items()
                    if key != 'content02.preparation.flat_map_storage'}
        self.assertEqual(len(ordinary), 8)
        for reference_id, example in ordinary.items():
            with self.subTest(reference_id=reference_id):
                result = self.compile_preparation(example)
                self.assertEqual(result.returncode, 0, result.stderr)

    def test_audit_help_articles_are_textual_and_addressed(self):
        root = ROOT / 'content/cpp/topics/containers/skills'
        expected = {
            'vector_reserve_does_not_change_size': 'audit20261126.vector_storage_trace',
            'vector_invalidation_multiple_choice': 'audit20261126.iterator_position_trace',
            'vector_reserve_bug_line': 'audit20261126.index_precondition_trace',
            'erase_remove_vector_size': 'audit20261126.logical_range_trace',
        }
        articles = {}
        bindings = {}
        for path in root.glob('*/materials*.json'):
            material = json.loads(path.read_text(encoding='utf-8'))
            for article in material['references']:
                if article['id'] in expected.values():
                    self.assertNotIn(article['id'], articles)
                    self.assertNotEqual(path.name, 'materials-preparation.json')
                    articles[article['id']] = article
            for binding in material['reference_bindings']:
                if binding['question_id'] in expected:
                    self.assertNotIn(binding['question_id'], bindings)
                    bindings[binding['question_id']] = binding['reference_ids']
        self.assertEqual(set(articles), set(expected.values()))
        self.assertEqual(bindings, {qid: [ref] for qid, ref in expected.items()})
        self.assertTrue(set(articles).isdisjoint(self.container_preparation_examples()))

    def test_move_fundamentals_primary_theory_compiles(self):
        example = theory_records()['move_semantics.cast_and_resource_transfer']['example']
        compiled = self.compile_preparation(example)
        self.assertEqual(compiled.returncode, 0, compiled.stderr)

    def test_move01_pre_answer_help_examples_compile(self):
        materials = ROOT / "content/cpp/topics/move_semantics/skills/move.fundamentals/materials.json"
        articles = json.loads(materials.read_text(encoding="utf-8"))["references"]
        self.assertEqual({article["id"] for article in articles},
                         {"move01.named_reference_category", "move01.noexcept_selection"})
        for article in articles:
            with self.subTest(article=article["id"]):
                compiled = self.compile_preparation(article["example"])
                self.assertEqual(compiled.returncode, 0, compiled.stderr)

    def test_move01_choices_do_not_encode_reasoning_in_the_right_option(self):
        expected = {
            "move_named_rvalue_reference_is_lvalue": ["MM", "CM", "CC", "Ошибка компиляции"],
            "move_if_noexcept_copy_fallback": ["M", "C", "CM", "Ошибка компиляции"],
        }
        for question_id, labels in expected.items():
            path = ROOT / "content/questions/move_semantics" / (question_id + ".json")
            card = json.loads(path.read_text(encoding="utf-8"))
            with self.subTest(question_id=question_id):
                self.assertEqual(card["text"], "Что выведет программа?")
                self.assertEqual([item["text"] for item in card["task"]["options"]], labels)
                self.assertIn(card["task"]["correct_option_id"],
                              {item["id"] for item in card["task"]["options"]})

    def test_question_quality_help_examples_compile(self):
        expected = {
            "qa20261124.algorithm_constraints",
            "qa20261124.automatic_scope_check",
            "qa20261124.map_probe_contract",
        }
        examples = {}
        for path in (ROOT / "content/cpp/topics").glob("*/skills/*/materials*.json"):
            for entry in json.loads(path.read_text(encoding="utf-8"))["references"]:
                if entry["id"] in expected:
                    self.assertNotIn(entry["id"], examples)
                    examples[entry["id"]] = entry["example"]
        self.assertEqual(set(examples), expected)
        for reference_id, example in examples.items():
            with self.subTest(reference_id=reference_id):
                compiled = self.compile_preparation(example)
                self.assertEqual(compiled.returncode, 0, compiled.stderr)

    def test_weak_ptr_distractors_share_cpp_form(self):
        question = json.loads((ROOT / "content/questions/lifetime/"
                               "weak_ptr_lock_is_atomic_access.json").read_text(encoding="utf-8"))
        self.assertEqual(question["task"]["correct_option_id"], "c")
        options = question["task"]["options"]
        self.assertEqual(len(options), 4)
        for option in options:
            self.assertTrue(option["text"].startswith("if ("), option["id"])
            code = ("#include <memory>\nstruct Widget { void use(); };\n"
                    "void test(const std::weak_ptr<Widget>& cached) {\n"
                    + option["text"] + "\n}\n")
            result = self.compile_preparation(code)
            self.assertEqual(result.returncode, 0, result.stderr)

    def test_ranges_owner_distractors_do_not_spoil_fix(self):
        question = json.loads((ROOT / "content/questions/containers/"
                               "ranges_search_local_owner_return.json").read_text(encoding="utf-8"))
        self.assertEqual(question["task"]["correct_option_id"], "a")
        self.assertEqual(len(question["task"]["options"]), 4)
        self.assertFalse(any("optional" in option["text"].lower() for option in question["task"]["options"]))
        result = self.compile_preparation(question["code"])
        self.assertEqual(result.returncode, 0, result.stderr)

    def test_flat_map_preparation_example_compiles_when_supported(self):
        probe = self.compile_preparation('''#include <flat_map>
#if !defined(__cpp_lib_flat_map) || __cpp_lib_flat_map < 202207L
#error flat_map feature is unavailable
#endif
std::flat_map<int, int> probe;
''')
        if probe.returncode != 0:
            self.skipTest('C++23 flat_map unavailable in selected standard library; '
                          'normative review remains required')
        example = self.container_preparation_examples()['content02.preparation.flat_map_storage']
        result = self.compile_preparation(example)
        self.assertEqual(result.returncode, 0, result.stderr)

    @staticmethod
    def container_preparation_examples():
        examples = {}
        for path in (ROOT / 'content/cpp/topics/containers/skills').glob('*/materials-preparation.json'):
            for reference in json.loads(path.read_text(encoding='utf-8'))['references']:
                if reference['id'] in examples:
                    raise ValueError('duplicate preparation reference: ' + reference['id'])
                examples[reference['id']] = reference['example']
        return examples

    @staticmethod
    def compile_preparation(example):
        # Some examples deliberately declare external functions: do not link or
        # execute them, and never execute a question's undefined-behaviour path.
        with tempfile.TemporaryDirectory(prefix='cpp-recall-preparation-example-') as directory:
            source = Path(directory) / 'example.cc'
            source.write_text(example, encoding='utf-8')
            return subprocess.run([COMPILER, '-std=c++23', '-Wall', '-Wextra',
                                   '-pedantic-errors', '-fsyntax-only', str(source)],
                                  text=True, capture_output=True, timeout=30)

    def test_slicing_value_reference_uses_complete_actual_card(self):
        card = json.loads((ROOT / 'content/questions/oop/'
                           'oop_slicing_value_vs_reference.json').read_text())
        with tempfile.TemporaryDirectory(prefix='cpp-recall-slicing-') as directory:
            source = Path(directory) / 'example.cc'
            binary = Path(directory) / 'example'
            source.write_text(card['code'], encoding='utf-8')
            compiled = subprocess.run([COMPILER, '-std=c++23', '-Wall', '-Wextra',
                                       '-pedantic-errors', str(source), '-o', str(binary)],
                                      text=True, capture_output=True, timeout=20)
            self.assertEqual(compiled.returncode, 0, compiled.stderr)
            result = subprocess.run([str(binary)], text=True, capture_output=True, timeout=5)
            self.assertEqual((result.returncode, result.stdout), (0, 'BD'), result.stderr)
        option = next(option for option in card['task']['options']
                      if option['id'] == card['task']['correct_option_id'])
        self.assertEqual(option['text'], 'BD')

    def test_raii_unique_ptr_exception_trace_uses_actual_card(self):
        card = json.loads((ROOT / 'content/questions/language_core/'
                           'raii_unique_ptr_caught_exception.json').read_text())
        with tempfile.TemporaryDirectory(prefix='cpp-recall-owner-exception-') as directory:
            source = Path(directory) / 'example.cc'
            binary = Path(directory) / 'example'
            source.write_text(card['code'], encoding='utf-8')
            compiled = subprocess.run([COMPILER, '-std=c++23', '-Wall', '-Wextra',
                                       '-pedantic-errors', str(source), '-o', str(binary)],
                                      text=True, capture_output=True, timeout=20)
            self.assertEqual(compiled.returncode, 0, compiled.stderr)
            result = subprocess.run([str(binary)], text=True, capture_output=True, timeout=5)
            self.assertEqual((result.returncode, result.stdout), (0, 'ARC'), result.stderr)
            # Supplementary execution, not proof of the N4950 unwinding rule.

    def test_raii_lock_guard_card_is_a_complete_cpp23_example(self):
        # Catches the former undeclared update_state/missing-header snippet.
        # The mutex/unwinding claim is reviewed against N4950, not inferred from
        # a compiler accepting the program.
        card = json.loads((ROOT / 'content/questions/language_core/'
                           'raii_lock_guard_unlocks_during_unwinding.json').read_text())
        with tempfile.TemporaryDirectory(prefix='cpp-recall-lock-guard-') as directory:
            source = Path(directory) / 'example.cc'
            source.write_text(card['code'], encoding='utf-8')
            result = subprocess.run([COMPILER, '-std=c++23', '-Wall', '-Wextra',
                                     '-pedantic-errors', '-fsyntax-only', str(source)],
                                    text=True, capture_output=True, timeout=20)
            self.assertEqual(result.returncode, 0, result.stderr)

    def test_map_operation_contract_review(self):
        # The example is a contract scenario, not code to execute. Pin the whole
        # reviewed text and its explicit coverage without treating SHA as proof
        # of technical correctness or independent pedagogical acceptance.
        ledger = json.loads((ROOT / 'docs/reviews/theory/2026-11-05-map-operation-contract.json').read_text())
        self.assertEqual(ledger['reviewer']['mode'], 'self_review')
        self.assertFalse(ledger['reviewer']['independent'])
        record = ledger['reviews'][0]
        snapshot = theory_records()['map_lookup.operation_contract']
        self.assertEqual({key: record[key] for key in snapshot}, snapshot)
        canonical = json.dumps(snapshot, ensure_ascii=False, sort_keys=True, separators=(',', ':'))
        self.assertEqual(record['reviewed_content_sha256'], hashlib.sha256(canonical.encode()).hexdigest())
        self.assertTrue(all(source.startswith('https://timsong-cpp.github.io/cppwp/n4950/')
                            for source in snapshot['sources']))

        reviewed = {entry['question_id']: entry for entry in ledger['card_review']}
        self.assertEqual(len(reviewed), len(ledger['card_review']))
        cards = {}
        for path in (ROOT / 'content/questions').rglob('*.json'):
            card = json.loads(path.read_text())
            # This historical review pins its original thirteen-card cohort.
            # Later cards have their own dated technical/relevance reviews.
            if card['id'] in reviewed:
                self.assertEqual(card.get('skill_id'), 'containers.map_lookup')
                cards[card['id']] = card
        self.assertEqual(set(reviewed), set(cards), 'repeat map pairing review when its card set changes')
        scope = ledger['binding_scope']
        self.assertFalse(scope['primary'])
        included = set(scope['included_families'])
        excluded = set(scope['excluded_families'])
        self.assertFalse(included & excluded)
        self.assertEqual({card['question_family_id'] for card in cards.values()}, included | excluded)
        self.assertEqual((len(cards), len(included), len(excluded)), (13, 5, 2))
        for question_id, card in cards.items():
            with self.subTest(question_id=question_id):
                entry = reviewed[question_id]
                for field in ('public_id', 'question_family_id', 'standard'):
                    self.assertEqual(entry[field], card[field])
                self.assertEqual(entry['post_error_principle'], card['question_family_id'] in included)
        self.assertEqual(sum(entry['post_error_principle'] for entry in reviewed.values()), 11)

    def test_map_qa_help_does_not_name_neighboring_answers(self):
        ledger = json.loads((ROOT / 'docs/reviews/theory/2026-11-24-map-qa-batch02.json').read_text())
        record = ledger['reviews'][0]
        snapshot = theory_records()['qa20261124.map_contract_checklist']
        self.assertEqual({key: record[key] for key in snapshot}, snapshot)
        canonical = json.dumps(snapshot, ensure_ascii=False, sort_keys=True, separators=(',', ':'))
        self.assertEqual(record['reviewed_content_sha256'], hashlib.sha256(canonical.encode()).hexdigest())
        material = json.loads((ROOT / 'content/cpp/topics/containers/skills/containers.map_lookup/materials.json').read_text())
        self.assertEqual(len(ledger['bindings']), 5)
        self.assertEqual({b['question_family_id'] for b in ledger['bindings']},
                         {b['question_family_id'] for b in material['theory_bindings']})
        self.assertTrue(all(b['theory_id'] == 'qa20261124.map_contract_checklist' for b in material['theory_bindings']))
        hints = [a for a in material['references'] if a['id'] in
                 ('qa20261124.map_lookup_checklist', 'qa20261124.map_mutation_checklist')]
        self.assertEqual(len(hints), 2)
        for article in [snapshot, *hints]:
            text = ' '.join(article[key] for key in
                            ('title', 'principle', 'mechanism', 'example', 'common_mistake'))
            for method in ('find(', 'contains(', 'try_emplace', 'insert_or_assign', 'operator[]', 'at('):
                self.assertNotIn(method, text)
        self.assertEqual(len(material['reference_bindings']), 11)
        self.assertTrue(all(b['reference_ids'] in (['qa20261124.map_lookup_checklist'], ['qa20261124.map_mutation_checklist'])
                            for b in material['reference_bindings']))

    def test_unordered_qa_exact_theory_and_help_scope(self):
        ledger = json.loads((ROOT / 'docs/reviews/theory/2026-11-24-unordered-qa-batch03.json').read_text())
        materials = ROOT / 'content/cpp/topics/containers/skills/containers.unordered/'
        main = json.loads((materials / 'materials-content02.json').read_text())
        prep = json.loads((materials / 'materials-preparation.json').read_text())
        snapshots = theory_records()
        for record in ledger['reviews']:
            snapshot = snapshots[record['id']]
            self.assertEqual({key: record[key] for key in snapshot}, snapshot)
            canonical = json.dumps(snapshot, ensure_ascii=False, sort_keys=True, separators=(',', ':'))
            self.assertEqual(record['reviewed_content_sha256'], hashlib.sha256(canonical.encode()).hexdigest())
        self.assertEqual(len(ledger['bindings']), 2)
        by_family = {b['question_family_id']: b['theory_id'] for b in main['theory_bindings']}
        for binding in ledger['bindings']:
            self.assertEqual(by_family[binding['question_family_id']], binding['theory_id'])
        by_question = {b['question_id']: b['reference_ids'] for b in
                       main['reference_bindings'] + prep['reference_bindings']}
        self.assertEqual(by_question, {"unordered_hash_equality_contract":["content02.hash_equivalence.reference"],"unordered_map_iterator_member_address_completion":["qa20261124.unordered_member_path"],"unordered_map_reserve_vs_rehash":["content02.preparation.hash_table_units"],"unordered_pointer_rehash_erase_review":["qa20261124.unordered_handle_check"],"lru_insert_failure_atomicity_review":["qa20261124.unordered_commit_check"]})
        for question_id in ('unordered_map_iterator_member_address_completion', 'unordered_map_reserve_vs_rehash', 'lru_insert_failure_atomicity_review'):
            card = json.loads((ROOT / 'content/questions/containers' / (question_id + '.json')).read_text())
            explanation = card['explanation'].lower()
            self.assertNotIn('rehash инвалидирует итераторы', explanation)
            self.assertNotIn('rehash не делает этот указатель', explanation)
        # A new method hint is not a ready-made solution to the adjacent card.
        narrow_ids = {'qa20261124.unordered_member_path', 'qa20261124.unordered_handle_check', 'qa20261124.unordered_commit_check'}
        narrow = [a for a in main['references'] if a['id'] in narrow_ids]
        self.assertEqual({a['id'] for a in narrow}, narrow_ids)
        for article in narrow:
            body = ' '.join(article[k] for k in ('principle', 'mechanism', 'example', 'common_mistake'))
            self.assertNotIn('it->second.value', body)
            self.assertNotIn('rehash инвалидирует итераторы', body.lower())

    def test_unordered_qa_safe_examples(self):
        root = ROOT / 'content/questions/containers'
        def card(name):
            return json.loads((root / (name + '.json')).read_text())
        # Only evaluate Hash/Equal directly, never create the invalid table.
        hashing = card('unordered_hash_equality_contract')['code']
        address = card('unordered_map_iterator_member_address_completion')['code'].replace(
            '/* допишите выражение */', '&it->second.value')
        safe = hashing + '\n' + address + '''
#include <cassert>
int main() {
  Key a{7, 1}, b{7, 2};
  assert(Equal{}(a, b) && Hash{}(a) != Hash{}(b));
  Cache empty;
  assert(empty.TryGet("missing") == nullptr);
  std::unordered_map<int, std::string> values{{7, "ready"}, {9, "other"}};
  const auto* saved = &values.find(7)->second;
  const auto old_count = values.bucket_count();
  values.rehash(old_count + 1);
  assert(values.bucket_count() > old_count);
  assert(saved == &values.find(7)->second && *saved == "ready");
  values.erase(9);
  assert(*saved == "ready");
  values.erase(7);
  assert(!values.contains(7)); // Never use saved after erase.
  std::unordered_map<int, int> table{{1, 1}, {2, 2}};
  table.max_load_factor(0.5f);
  table.reserve(10);
  assert(table.bucket_count() >= 20 && table.size() == 2);
  table.rehash(50);
  assert(table.bucket_count() >= 50 && table.size() == 2);
}
'''
        with tempfile.TemporaryDirectory(prefix='cpp-recall-unordered-qa-') as directory:
            source = Path(directory) / 'example.cc'
            binary = Path(directory) / 'example'
            source.write_text(safe, encoding='utf-8')
            compiled = subprocess.run([COMPILER, '-std=c++23', '-Wall', '-Wextra',
                                       '-pedantic-errors', str(source), '-o', str(binary)],
                                      text=True, capture_output=True, timeout=30)
            self.assertEqual(compiled.returncode, 0, compiled.stderr)
            result = subprocess.run([str(binary)], text=True, capture_output=True, timeout=5)
            self.assertEqual(result.returncode, 0, result.stderr)

    def test_stl_qa_help_and_theory(self):
        root = ROOT / 'content/cpp/topics/containers/skills/algorithms.stl/'
        basic = json.loads((root / 'materials.json').read_text())
        advanced = json.loads((root / 'materials-content02.json').read_text())
        bindings = {b['question_id']: b['reference_ids'] for b in
                    basic['reference_bindings'] + advanced['reference_bindings']}
        expected = {"lower_bound_partition_precondition":["qa20261125.boundary_trace"],"lower_bound_first_equal_position":["qa20261125.boundary_trace"],"lower_bound_descending_boundary":["qa20261125.boundary_trace"],"lower_bound_candidate_not_match":["qa20261125.boundary_trace"],"comparator_equivalence_not_equality":["qa20261125.order_pair_trace"],"ranges_sort_member_projection":["qa20261125.projection_trace"],"stl_algorithms_matching":["qa20261125.algorithm_contract_check"],"comparator_cycle_counterexample":["content02.ordering_contract.reference"]}
        self.assertEqual({key: bindings[key] for key in expected}, expected)
        ledger = json.loads((ROOT / 'docs/reviews/theory/2026-11-25-stl-qa-batch04.json').read_text())
        record = ledger['reviews'][0]
        snapshot = theory_records()['qa20261125.order_counterexample']
        self.assertEqual({key: record[key] for key in snapshot}, snapshot)
        canonical = json.dumps(snapshot, ensure_ascii=False, sort_keys=True, separators=(',', ':'))
        self.assertEqual(record['reviewed_content_sha256'], hashlib.sha256(canonical.encode()).hexdigest())
        actual = next(b for b in advanced['theory_bindings']
                      if b['question_family_id'] == 'algorithms.stl.strict_weak_order_validity')
        self.assertEqual(actual['theory_id'], 'qa20261125.order_counterexample')

    def test_stl_qa_partition_and_cycle(self):
        card = json.loads((ROOT / 'content/questions/containers/comparator_cycle_counterexample.json').read_text())
        example = card['code'] + '''
#include <algorithm>
#include <cassert>
#include <forward_list>
int main() {
  std::forward_list<int> values{3, 1, 8, 6};
  auto it = std::lower_bound(values.begin(), values.end(), 5);
  assert(it != values.end() && *it == 8);
  assert(!Before(0, 0) && Before(0, 1) && !Before(1, 0));
  assert(Before(2, 0) && Before(1, 2) && !Before(0, 2));
}
'''
        # The comparator is tested directly; never passed to sort.
        with tempfile.TemporaryDirectory(prefix='cpp-recall-stl-qa-') as directory:
            source = Path(directory) / 'example.cc'
            binary = Path(directory) / 'example'
            source.write_text(example, encoding='utf-8')
            compiled = subprocess.run([COMPILER, '-std=c++23', '-Wall', '-Wextra',
                                       '-pedantic-errors', str(source), '-o', str(binary)],
                                      text=True, capture_output=True, timeout=30)
            self.assertEqual(compiled.returncode, 0, compiled.stderr)
            result = subprocess.run([str(binary)], text=True, capture_output=True, timeout=5)
            self.assertEqual(result.returncode, 0, result.stderr)

    def test_composed_qa_exact_help(self):
        root = ROOT / 'content/cpp/topics/containers/skills'
        materials = [json.loads((root / skill / 'materials-content02.json').read_text())
                     for skill in ('algorithms.stl', 'containers.map_lookup')]
        bindings = {b['question_id']: b['reference_ids'] for m in materials
                    for b in m['reference_bindings']}
        expected = {
            'stable_sort_equivalent_order': ['qa20261125.stable_order_trace'],
            'stable_sort_multi_key_review': ['qa20261125.multi_key_trace'],
            'map_interval_query_review': ['qa20261125.interval_position_trace'],
            'snapshot_owns_projected_keys_review': ['qa20261125.snapshot_owner_trace'],
            'deduplicate_first_seen_contract_review': ['qa20261125.dedup_trace'],
        }
        self.assertEqual({key: bindings[key] for key in expected}, expected)
        family = {b['question_family_id']: b['theory_id'] for m in materials
                  for b in m['theory_bindings']}
        ledger = json.loads((ROOT / 'docs/reviews/theory/2026-11-25-composed-qa-batch05.json').read_text())
        snapshots = theory_records()
        self.assertEqual(len(ledger['reviews']), 4)
        self.assertEqual(len(ledger['bindings']), 4)
        for record in ledger['reviews']:
            snapshot = snapshots[record['id']]
            self.assertEqual({key: record[key] for key in snapshot}, snapshot)
            encoded = json.dumps(snapshot, ensure_ascii=False, sort_keys=True, separators=(',', ':'))
            self.assertEqual(record['reviewed_content_sha256'], hashlib.sha256(encoded.encode()).hexdigest())
        expected_theory = {
            'algorithms.stl.stable_sort_equivalent_order': 'qa20261125.stable_order_principle',
            'containers.map_lookup.interval_query': 'qa20261125.interval_position_principle',
            'algorithms.stl.owning_sorted_snapshot': 'qa20261125.snapshot_owner_principle',
            'algorithms.stl.deduplication_contract': 'qa20261125.dedup_principle',
        }
        self.assertEqual({key: family[key] for key in expected_theory}, expected_theory)
        self.assertEqual({b['question_family_id']: b['theory_id'] for b in ledger['bindings']},
                         expected_theory)

    def test_views_qa_exact_help(self):
        materials = []
        for path in (ROOT / 'content/cpp/topics').glob('*/skills/*/materials*.json'):
            materials.append(json.loads(path.read_text(encoding='utf-8')))
        bindings = {b['question_id']: b['reference_ids'] for m in materials
                    for b in m['reference_bindings']}
        expected = {
            'const_span_mutable_elements': ['qa20261125.span_const_trace'],
            'ranges_find_rvalue_vector_result': ['qa20261125.borrowed_result_trace'],
            'ranges_search_local_owner_return': ['qa20261125.local_owner_trace'],
            'ranges_sort_list_constraints': ['qa20261124.algorithm_constraints'],
            'span_after_vector_reallocation': ['qa20261125.span_address_trace'],
            'span_observes_owner_changes': ['qa20261125.span_alias_trace'],
            'span_owner_mutation_without_reallocation': ['qa20261125.span_extent_trace'],
            'string_view_from_temporary_string': ['qa20261125.string_owner_trace'],
        }
        self.assertEqual({key: bindings[key] for key in expected}, expected)
        family = {b['question_family_id']: b['theory_id'] for m in materials
                  for b in m['theory_bindings']}
        ledger = json.loads((ROOT / 'docs/reviews/theory/2026-11-25-views-qa-batch06.json').read_text())
        snapshots = theory_records()
        self.assertEqual(len(ledger['reviews']), 2)
        self.assertEqual(len(ledger['bindings']), 2)
        for record in ledger['reviews']:
            snapshot = snapshots[record['id']]
            self.assertEqual({key: record[key] for key in snapshot}, snapshot)
            encoded = json.dumps(snapshot, ensure_ascii=False, sort_keys=True, separators=(',', ':'))
            self.assertEqual(record['reviewed_content_sha256'], hashlib.sha256(encoded.encode()).hexdigest())
        expected_theory = {
            'library.non_owning_views.span_after_vector_reallocation': 'qa20261125.view_validity_principle',
            'ranges.fundamentals.ranges_find_rvalue_vector_result': 'qa20261125.range_owner_principle',
        }
        self.assertEqual({key: family[key] for key in expected_theory}, expected_theory)
        self.assertEqual({b['question_family_id']: b['theory_id'] for b in ledger['bindings']},
                         expected_theory)

    def test_final_container_qa_exact_help(self):
        materials = []
        for path in (ROOT / 'content/cpp/topics').glob('*/skills/*/materials*.json'):
            materials.append(json.loads(path.read_text(encoding='utf-8')))
        bindings = {b['question_id']: b['reference_ids'] for m in materials
                    for b in m['reference_bindings']}
        expected = {
            'map_node_collision_preserves_payload': ['qa20261125.node_collision_trace'],
            'map_node_handle_change_key': ['qa20261125.node_detached_trace'],
            'flat_map_min_dangling_reference': ['content02.preparation.flat_map_storage'],
            'vector_append_selected_transaction_review': ['qa20261125.append_contract_trace'],
            'vector_filter_transaction_review': ['qa20261125.filter_exception_trace'],
            'invoke_const_member_function': ['qa20261125.invoke_method_trace'],
            'invoke_member_data_write': ['qa20261125.invoke_member_trace'],
            'optional_value_or_eager_argument': ['content02.preparation.argument_or_action'],
        }
        self.assertEqual({key: bindings[key] for key in expected}, expected)
        family = {b['question_family_id']: b['theory_id'] for m in materials
                  for b in m['theory_bindings']}
        ledger = json.loads((ROOT / 'docs/reviews/theory/2026-11-25-container-final-qa-batch07.json').read_text())
        snapshots = theory_records()
        self.assertEqual(len(ledger['reviews']), 2)
        self.assertEqual(len(ledger['bindings']), 2)
        for record in ledger['reviews']:
            snapshot = snapshots[record['id']]
            self.assertEqual({key: record[key] for key in snapshot}, snapshot)
            encoded = json.dumps(snapshot, ensure_ascii=False, sort_keys=True, separators=(',', ':'))
            self.assertEqual(record['reviewed_content_sha256'], hashlib.sha256(encoded.encode()).hexdigest())
        expected_theory = {
            'containers.map_lookup.map_node_handle_change_key': 'qa20261125.node_ownership_principle',
            'containers.vector.transactional_transform': 'qa20261125.transaction_contract_principle',
        }
        self.assertEqual({key: family[key] for key in expected_theory}, expected_theory)
        self.assertEqual({b['question_family_id']: b['theory_id'] for b in ledger['bindings']},
                         expected_theory)

    def test_lifetime_qa_exact_help(self):
        materials = []
        for path in (ROOT / 'content/cpp/topics').glob('*/skills/*/materials*.json'):
            materials.append(json.loads(path.read_text(encoding='utf-8')))
        bindings = {b['question_id']: b['reference_ids'] for m in materials
                    for b in m['reference_bindings']}
        expected = {
            'aggregate_reference_paren_vs_brace': ['qa20261125.aggregate_initializer_trace'],
            'c_str_from_temporary_dangles': ['qa20261125.cstr_access', 'qa20261125.string_owner_trace'],
            'const_reference_extends_temporary': ['qa20261125.temporary_binding_trace'],
            'lifetime_definition': [],
            'optional_emplace_starts_object_lifetime': ['qa20261125.object_storage_trace'],
            'placement_new_starts_new_lifetime': ['qa20261125.object_storage_trace'],
            'pointer_after_local_lifetime': ['qa20261125.local_scope_trace'],
            'pointer_reference_share_object': ['qa20261125.live_alias_trace'],
            'return_reference_does_not_extend_temporary': ['qa20261125.returned_owner_trace'],
            'return_rvalue_reference_to_local': ['qa20261125.returned_owner_trace'],
            'const_reference_observes_changes': ['qa20261125.reference_const_trace'],
            'reference_alias_and_copy': ['qa20261125.reference_identity_trace'],
            'reference_definition': ['qa20261125.reference_identity_trace'],
            'reference_member_assignment': ['qa20261125.reference_identity_trace'],
            'reference_parameter_updates_argument': ['qa20261125.reference_parameter_trace'],
            'reference_return_existing_member': ['qa20261125.reference_result_trace'],
        }
        self.assertEqual({key: bindings.get(key, []) for key in expected}, expected)
        family = {b['question_family_id']: b['theory_id'] for m in materials
                  for b in m['theory_bindings']}
        ledger = json.loads((ROOT / 'docs/reviews/theory/2026-11-25-lifetime-references-qa-batch08.json').read_text())
        snapshots = theory_records()
        self.assertEqual(len(ledger['reviews']), 5)
        self.assertEqual(len(ledger['bindings']), 14)
        for record in ledger['reviews']:
            snapshot = snapshots[record['id']]
            self.assertEqual({key: record[key] for key in snapshot}, snapshot)
            encoded = json.dumps(snapshot, ensure_ascii=False, sort_keys=True, separators=(',', ':'))
            self.assertEqual(record['reviewed_content_sha256'], hashlib.sha256(encoded.encode()).hexdigest())
        expected_theory = {
            'lifetime.objects.aggregate_reference_extension': 'qa20261125.temporary_timeline_principle',
            'lifetime.objects.non_owning_temporary': 'qa20261125.temporary_timeline_principle',
            'lifetime.objects.temporary_extension': 'qa20261125.temporary_timeline_principle',
            'lifetime.objects.concept': 'qa20261125.storage_object_principle',
            'lifetime.objects.explicit_lifetime_start': 'qa20261125.storage_object_principle',
            'lifetime.objects.local_escape': 'qa20261125.live_access_principle',
            'lifetime.objects.borrow_alias': 'qa20261125.live_access_principle',
            'lifetime.objects.returned_reference': 'qa20261125.temporary_timeline_principle',
            'references.fundamentals.const_access_not_snapshot': 'qa20261125.reference_access_principle',
            'references.fundamentals.alias_and_scalar_copy': 'qa20261125.reference_access_principle',
            'references.fundamentals.reference_definition': 'qa20261125.reference_access_principle',
            'references.fundamentals.reference_member_assignment': 'qa20261125.reference_access_principle',
            'references.fundamentals.value_and_reference_parameters': 'qa20261125.reference_access_principle',
            'references.fundamentals.return_external_member': 'qa20261125.returned_access_principle',
        }
        self.assertEqual({key: family[key] for key in expected_theory}, expected_theory)
        self.assertEqual({b['question_family_id']: b['theory_id'] for b in ledger['bindings']},
                         expected_theory)

    def test_smart_pointer_qa_exact_help(self):
        materials = []
        for path in (ROOT / 'content/cpp/topics').glob('*/skills/*/materials*.json'):
            materials.append(json.loads(path.read_text(encoding='utf-8')))
        bindings = {b['question_id']: b['reference_ids'] for m in materials
                    for b in m['reference_bindings']}
        expected = {
            'shared_ptr_alias_keeps_owner_alive': ['qa20261125.alias_owner_trace'],
            'shared_ptr_copy_use_count': ['qa20261125.shared_count_trace'],
            'shared_ptr_cycle_requires_weak_back_reference': ['qa20261125.ownership_graph_trace'],
            'shared_ptr_duplicate_control_blocks': ['qa20261125.shared_group_trace'],
            'shared_ptr_same_instance_data_race': ['qa20261125.shared_concurrency_trace'],
            'smart_pointers_matching': ['smart_pointer_roles'],
            'unique_ptr_borrow_across_owner_move': ['qa20261125.unique_owner_transition'],
            'unique_ptr_borrow_without_transfer': ['qa20261125.borrow_api_trace'],
            'unique_ptr_make_get_observe': ['unique_ptr_access'],
            'unique_ptr_move_transfers_ownership': ['qa20261125.unique_move_trace'],
            'unique_ptr_reset_destroys_now': ['unique_ptr_reset'],
            'unique_ptr_scope_deletes_object': ['qa20261125.owner_scope_trace'],
            'weak_ptr_lock_is_atomic_access': ['qa20261125.weak_lifetime_trace'],
        }
        self.assertEqual({key: bindings.get(key, []) for key in expected}, expected)
        family = {b['question_family_id']: b['theory_id'] for m in materials
                  for b in m['theory_bindings']}
        ledger = json.loads((ROOT / 'docs/reviews/theory/2026-11-25-smart-pointers-qa-batch09.json').read_text())
        snapshots = theory_records()
        self.assertEqual(len(ledger['reviews']), 3)
        self.assertEqual(len(ledger['bindings']), 13)
        for record in ledger['reviews']:
            snapshot = snapshots[record['id']]
            self.assertEqual({key: record[key] for key in snapshot}, snapshot)
            encoded = json.dumps(snapshot, ensure_ascii=False, sort_keys=True, separators=(',', ':'))
            self.assertEqual(record['reviewed_content_sha256'], hashlib.sha256(encoded.encode()).hexdigest())
        expected_theory = {
            'lifetime.smart_pointers.shared_alias': 'qa20261125.shared_group_principle',
            'lifetime.smart_pointers.shared_count': 'qa20261125.shared_group_principle',
            'lifetime.smart_pointers.shared_cycle': 'qa20261125.shared_group_principle',
            'lifetime.smart_pointers.shared_control_block': 'qa20261125.shared_group_principle',
            'lifetime.smart_pointers.shared_concurrency': 'qa20261125.concurrent_owner_principle',
            'lifetime.smart_pointers.roles': 'qa20261125.shared_group_principle',
            'lifetime.smart_pointers.borrow_after_owner_transfer': 'qa20261125.unique_resource_principle',
            'lifetime.smart_pointers.unique_borrow': 'qa20261125.unique_resource_principle',
            'lifetime.smart_pointers.unique_access': 'qa20261125.unique_resource_principle',
            'lifetime.smart_pointers.unique_transfer': 'qa20261125.unique_resource_principle',
            'lifetime.smart_pointers.unique_reset': 'qa20261125.unique_resource_principle',
            'lifetime.smart_pointers.unique_scope': 'qa20261125.unique_resource_principle',
            'lifetime.smart_pointers.weak_lock': 'qa20261125.concurrent_owner_principle',
        }
        self.assertEqual({key: family[key] for key in expected_theory}, expected_theory)
        self.assertEqual({b['question_family_id']: b['theory_id'] for b in ledger['bindings']},
                         expected_theory)

    def test_raii_qa_exact_help(self):
        materials = []
        for path in (ROOT / 'content/cpp/topics').glob('*/skills/*/materials*.json'):
            materials.append(json.loads(path.read_text(encoding='utf-8')))
        bindings = {b['question_id']: b['reference_ids'] for m in materials
                    for b in m['reference_bindings']}
        expected = {
            'raii_closes_file_on_early_exit': ['qa20261125.file_exit_trace'],
            'raii_definition': ['qa20261125.raii_concept_trace'],
            'raii_lock_guard_unlocks_during_unwinding': ['qa20261125.mutex_unwind_trace'],
            'raii_member_destruction_dependency': ['qa20261125.member_order_trace'],
            'raii_move_assignment_releases_old_resource': ['qa20261125.resource_assignment_trace'],
            'raii_partial_construction_cleanup': ['qa20261125.partial_init_trace'],
            'raii_raw_owner_copy_double_delete': ['qa20261125.raw_owner_special_members'],
            'raii_reverse_local_destruction_order': ['qa20261125.local_cleanup_trace'],
            'raii_state_rollback_commit': ['scope_cleanup'],
            'raii_unique_ptr_caught_exception': ['qa20261125.exception_owner_trace'],
            'block_scope_destruction_order': ['qa20261125.local_cleanup_trace'],
        }
        self.assertEqual({key: bindings.get(key, []) for key in expected}, expected)
        family = {b['question_family_id']: b['theory_id'] for m in materials
                  for b in m['theory_bindings']}
        ledger = json.loads((ROOT / 'docs/reviews/theory/2026-11-25-raii-qa-batch10.json').read_text())
        snapshots = theory_records()
        self.assertEqual(len(ledger['reviews']), 3)
        self.assertEqual(len(ledger['bindings']), 6)
        for record in ledger['reviews']:
            snapshot = snapshots[record['id']]
            self.assertEqual({key: record[key] for key in snapshot}, snapshot)
            encoded = json.dumps(snapshot, ensure_ascii=False, sort_keys=True, separators=(',', ':'))
            self.assertEqual(record['reviewed_content_sha256'], hashlib.sha256(encoded.encode()).hexdigest())
        expected_theory = {
            'raii.fundamentals.concept': 'qa20261125.raii_lifetime_principle',
            'raii.fundamentals.exception_cleanup': 'qa20261125.raii_lifetime_principle',
            'raii.fundamentals.reverse_destruction': 'qa20261125.raii_dependency_principle',
            'raii.fundamentals.move_assignment': 'qa20261125.raii_special_members_principle',
            'raii.fundamentals.partial_construction': 'qa20261125.raii_dependency_principle',
            'raii.fundamentals.ownership_copy': 'qa20261125.raii_special_members_principle',
        }
        self.assertEqual({key: family[key] for key in expected_theory}, expected_theory)
        self.assertEqual({b['question_family_id']: b['theory_id'] for b in ledger['bindings']},
                         expected_theory)

    def test_review_revision_chain(self):
        # A second editorial revision must not erase the first review, fork,
        # repeat a revision, or accept an unreviewed final card.
        with tempfile.TemporaryDirectory(prefix='cpp-recall-review-chain-') as directory:
            root = Path(directory)
            script = root / 'scripts/validate_content_reviews.py'
            script.parent.mkdir()
            script.write_text((ROOT / 'scripts/validate_content_reviews.py').read_text(encoding='utf-8'),
                              encoding='utf-8')
            question_file = root / 'content/questions/card.json'
            question_file.parent.mkdir(parents=True)
            reviews = root / 'docs/reviews/content'
            reviews.mkdir(parents=True)
            cards = [dict(id='card', public_id='CR-test', skill_id='skill',
                          question_family_id='family', standard='C++23', source='source',
                          text=text) for text in ('first', 'second', 'third')]
            revisions = [hashlib.sha256(json.dumps(q, ensure_ascii=False, sort_keys=True,
                                                 separators=(',', ':')).encode()).hexdigest() for q in cards]

            def record(index):
                q = cards[index]
                return {**{key: q[key] for key in ('public_id', 'skill_id', 'question_family_id', 'standard')},
                        'question_id': 'card', 'content_revision': revisions[index], 'decision': 'accepted'}

            def write_review(index, previous=None, revision_index=None):
                document = {'reviews': [record(index if revision_index is None else revision_index)]}
                if previous is not None:
                    document['replaces_card_revisions'] = {'card': previous}
                (reviews / f'{index}.json').write_text(json.dumps(document), encoding='utf-8')

            def run(expected):
                result = subprocess.run([sys.executable, str(script)], capture_output=True, text=True, timeout=10)
                self.assertEqual(result.returncode, expected, result.stdout + result.stderr)

            question_file.write_text(json.dumps(cards[2]), encoding='utf-8')
            write_review(0)
            write_review(1, revisions[0])
            write_review(2, revisions[1])
            run(0)
            write_review(2, revisions[0])  # fork
            run(1)
            write_review(2, revisions[1], revision_index=0)  # cycle
            run(1)
            write_review(2, revisions[1])
            question_file.write_text(json.dumps(cards[1]), encoding='utf-8')
            run(1)

    def test_h3_reference_examples(self):
        # core_tests checks these fixtures against the compiled catalog and its
        # full content revision. Execute only the individually reviewed cases.
        ledger = json.loads((ROOT / 'docs/reviews/references/2026-10-26-bindings.json').read_text())
        examples = {record['id']: record['example'] for record in ledger['references']}
        cases = {
            'raii_automatic_owner_lifetime': ('#include <vector>\nint main() {\n', '\n}'),
            'remove_erase': ('', '\nint main() { assert(jobs.size() == 3); '
                             'assert(tail == jobs.begin() + 2); '
                             'assert(jobs[0].id == 2 && !jobs[0].done); '
                             'assert(jobs[1].id == 3 && !jobs[1].done); }'),
            'map_update': ('#include <map>\n', '\nint main() { assert(limits.size() == 1); '
                           'assert(result.second && result.first == limits.find(7)); '
                           'assert(result.first->second == 20); }'),
            'span': ('#include <span>\n', '\nint main() { assert(total == 12); '
                     'assert(readings[0] == 4 && readings[1] == 8); }'),
            'invoke_result': ('#include <type_traits>\n', '\nstatic_assert(std::is_same_v<R, double>); '
                              'int main() {}'),
            'remove_cvref': ('#include <type_traits>\n', '\nstatic_assert(std::is_same_v<A, long>); '
                             'static_assert(std::is_same_v<B, int*>); int main() {}'),
            'unique_ownership': ('#include <memory>\nint main() {\n',
                                 '\nassert(!owner && snapshot == 18); }'),
            'forwarding_reference_basics': ('#include <type_traits>\n', '''
static_assert(std::is_same_v<decltype(Relay<long>), void(long&&)>);
static_assert(std::is_same_v<decltype(Relay<long&>), void(long&)>);
static_assert(std::is_same_v<decltype(Store<long>), void(const long&&)>);
static_assert(std::is_same_v<decltype(&Box<long>::Set), void(Box<long>::*)(long&&)>);
using L = long&;
using R = long&&;
static_assert(std::is_same_v<L&, long&> && std::is_same_v<L&&, long&>);
static_assert(std::is_same_v<R&, long&> && std::is_same_v<R&&, long&&>);
int main() {}
'''),
        }
        with tempfile.TemporaryDirectory(prefix='cpp-recall-h3-') as directory:
            source = Path(directory) / 'example.cc'
            binary = Path(directory) / 'example'

            def check(code, *, standard='c++23', compiles=True):
                source.write_text('#include <cassert>\n' + code, encoding='utf-8')
                command = [COMPILER, '-std=' + standard, '-Wall', '-Wextra', '-pedantic-errors', str(source)]
                # Reject bad declarations without linking/executing them.
                command += ['-o', str(binary)] if compiles else ['-fsyntax-only']
                result = subprocess.run(command, capture_output=True, text=True, timeout=40, check=False)
                if not compiles:
                    self.assertNotEqual(result.returncode, 0, 'invalid forwarding declaration accepted')
                    return
                self.assertEqual(result.returncode, 0, result.stderr)
                executed = subprocess.run([str(binary)], capture_output=True, text=True, timeout=5, check=False)
                self.assertEqual(executed.returncode, 0, executed.stderr)
                self.assertEqual(executed.stdout, '')

            for article_id, (prefix, suffix) in cases.items():
                with self.subTest(article_id=article_id):
                    check(prefix + examples[article_id] + suffix)

            introduction = examples['forwarding_reference_basics']
            for standard in ('c++17', 'c++23'):
                with self.subTest(standard=standard):
                    check(cases['forwarding_reference_basics'][0] + introduction +
                          cases['forwarding_reference_basics'][1], standard=standard)
                    for bad_main in (
                        'int main() { long value = 1; Store(value); }',
                        'int main() { long value = 1; Box<long> box; box.Set(value); }',
                        'int main() { long value = 1; Relay<long>(value); }',
                        'int main() { long value = 1; long& & invalid = value; }',
                    ):
                        check(introduction + '\n' + bad_main, standard=standard, compiles=False)

            theory_ledger = json.loads((ROOT / 'docs/reviews/theory/2026-11-03-forwarding.json').read_text())
            snapshot = theory_records()['deduction.forwarding_reference']
            theory = snapshot['example']
            self.assertEqual(theory_ledger['reviews'][0]['example'], theory)
            # Pin the whole newly reviewed block, not just executable lines.
            record = theory_ledger['reviews'][0]
            self.assertEqual({key: record[key] for key in snapshot}, snapshot)
            canonical = json.dumps(snapshot, ensure_ascii=False, sort_keys=True, separators=(',', ':'))
            self.assertEqual(record['reviewed_content_sha256'], hashlib.sha256(canonical.encode()).hexdigest())
            check(theory)
            # Test both dispatch paths; the published example itself stays short.
            instrumented = '#include <cassert>\nint lvalues = 0, rvalues = 0;\n' + theory
            for old, new in {
                'void Handle(Parcel&) {}': 'void Handle(Parcel&) { ++lvalues; }',
                'void Handle(Parcel&&) {}': 'void Handle(Parcel&&) { ++rvalues; }',
                'Relay(parcel);': 'Relay(parcel); Relay(Parcel{}); assert(lvalues == 1 && rvalues == 1);',
            }.items():
                self.assertEqual(instrumented.count(old), 1, 'forwarding fixture instrumentation became stale')
                instrumented = instrumented.replace(old, new)
            check(instrumented, standard='c++17')
            check(instrumented)

    def test_help_principle_examples(self):
        # Compile authoring strings independently checked against compiled data.
        # Expected behavior has a separate N4950 reasoning record in the patch
        # review; a passing compiler run is not independent content acceptance.
        catalog = theory_records()

        def example(block_id):
            self.assertIn(block_id, catalog, 'missing theory example: ' + block_id)
            return catalog[block_id]['example']

        examples = {
            'storage_duration.initialization_order': ('measured\n160 240\n', ''),
            'copy_construction.object_lifecycle': (
                'new\ncopy\n4 7\nclosed 7\n4\nclosed 4\n', ''),
            'raii.scope_cleanup': ('4\n', ''),
            'lifetime.references': ('', '\nint main() { assert(prepare() == 18); }\n'),
        }
        with tempfile.TemporaryDirectory(prefix='cpp-recall-help-principles-') as directory:
            source = Path(directory) / 'example.cc'
            binary = Path(directory) / 'example'
            for block_id, (output, suffix) in examples.items():
                with self.subTest(block_id=block_id):
                    code = example(block_id)
                    if block_id == 'copy_construction.object_lifecycle':
                        code = code.replace(
                            'const Draft& view = original;',
                            'const Draft& view = original; assert(&view == &original);')
                        code = code.replace(
                            'revision.pages = 7;',
                            'revision.pages = 7; assert(&revision != &original); '
                            'assert(original.pages == 4 && view.pages == 4);')
                    source.write_text('#include <cassert>\n' + code + suffix, encoding='utf-8')
                    compiled = subprocess.run(
                        [COMPILER, '-std=c++23', '-Wall', '-Wextra', '-pedantic-errors',
                         str(source), '-o', str(binary)], capture_output=True, text=True,
                        timeout=40, check=False)
                    self.assertEqual(compiled.returncode, 0, compiled.stderr)
                    executed = subprocess.run([str(binary)], capture_output=True,
                                              text=True, timeout=5, check=False)
                    self.assertEqual(executed.returncode, 0, executed.stderr)
                    self.assertEqual(executed.stdout, output)

    def test_a12_card_examples(self):
        # Only examples established as well-defined by the N4950 review run.
        # Invalid examples are compiled but never executed; tail values after
        # remove and unspecified cross-TU initialization order are not sampled.
        bank = {q['id']: q for p in (ROOT / 'content/questions').rglob('*.json')
                for q in [json.loads(p.read_text(encoding='utf-8'))]}
        with tempfile.TemporaryDirectory(prefix='cpp-recall-a12-') as directory:
            source = Path(directory) / 'example.cc'
            binary = Path(directory) / 'example'

            def check(code, *, output='', compiles=True):
                source.write_text('#include <cassert>\n' + code, encoding='utf-8')
                result = subprocess.run(
                    [COMPILER, '-std=c++23', '-Wall', '-Wextra', '-pedantic-errors',
                     str(source), '-o', str(binary)], capture_output=True, text=True,
                    timeout=40, check=False)
                if not compiles:
                    self.assertNotEqual(result.returncode, 0, 'ill-formed example compiled')
                    return
                self.assertEqual(result.returncode, 0, result.stderr)
                executed = subprocess.run([str(binary)], capture_output=True, text=True,
                                          timeout=5, check=False)
                self.assertEqual(executed.returncode, 0, executed.stderr)
                self.assertEqual(executed.stdout, output)

            for question_id, output in {
                'block_scope_destruction_order': 'BCA',
                'constrained_overload_selection': '21',
                'copy_constructor_const_ref_non_const_source': '1233',
                'forwarding_reference_lvalue_deduction': '10',
            }.items():
                with self.subTest(question_id=question_id):
                    check(bank[question_id]['code'], output=output)

            copy_code = bank['copy_constructor_non_const_ref_const_source']['code']
            check(copy_code, compiles=False)
            # Establish that const default-initialization of empty X is not
            # itself the reason for rejecting this card's program.
            check(copy_code.replace('X(X&)', 'X(const X&)'), output='1')

            member = bank['dependent_member_template_keyword']
            factory = 'struct Factory { template <typename T> T make() { return T{7}; } };\n'
            main = '\nint main() { Factory factory; assert(make_int(factory) == 7); }'
            check(factory + member['code'] + main, compiles=False)
            answer = next(o['text'] for o in member['task']['options']
                          if o['id'] == member['task']['correct_option_id'])
            check(factory + member['code'].replace('return factory.make<int>();', answer) + main)

            dependent = bank['dependent_type_requires_typename']
            suffix = '\nstruct Sample { using value_type = int; }; int main() { reset<Sample>(); }'
            check(dependent['code'] + suffix, compiles=False)
            answer = next(o['text'] for o in dependent['task']['options']
                          if o['id'] == dependent['task']['correct_option_id'])
            check(dependent['code'].replace('T::value_type value{};', answer) + suffix)

            widget = '''
struct Widget {
  static inline int runs = 0;
  static inline int destructions = 0;
  void run() { ++runs; }
  ~Widget() { ++destructions; }
};
int main() {
'''
            check(widget + bank['dynamic_object_outlives_pointer_scope']['code'] +
                  '\nassert(Widget::runs == 1 && Widget::destructions == 1); }')

            check(bank['erase_remove_vector_size']['code'] + '''
int main() {
  assert(values.size() == 4 && new_end == values.begin() + 2);
  assert(values[0] == 2 && values[1] == 3);
  values.erase(new_end, values.end());
  assert((values == std::vector<int>{2, 3}));
  std::vector<int> other{1, 2, 1, 3};
  assert(std::erase(other, 1) == 2);
  assert(other == values);
}
''')
            check(bank['extern_c_language_linkage']['code'] + '\nint main() {}')
            check('''
struct Counter { mutable int hits = 0; int value = 2; };
int main() {
  const Counter counter;
  ++counter.hits;
  assert(counter.hits == 1 && counter.value == 2);
  int original = 3;
  const int& view = original;
  original = 8;
  assert(view == 8);
  struct Holder { int* pointer; };
  Holder first{&original};
  Holder second = first;
  assert(&first != &second && first.pointer == second.pointer);
}
''')
            check('struct Counter { int value = 2; }; '
                  'int main() { const Counter counter; counter.value = 3; }', compiles=False)

    def test_a12_construct_on_first_use(self):
        # Exercise only the corrected design, in both link-input orders.
        # The original initialization-order hazard is not executed.
        with tempfile.TemporaryDirectory(prefix='cpp-recall-a12-static-') as directory:
            folder = Path(directory)
            header = folder / 'fixture.h'
            header.write_text('''
#ifndef CPP_RECALL_A12_FIXTURE_H
#define CPP_RECALL_A12_FIXTURE_H
struct Config { int value; };
Config& config();
struct Logger {
  int value;
  explicit Logger(const Config& source) : value(source.value) {}
};
extern Logger logger;
#endif
''', encoding='utf-8')
            config = folder / 'config.cc'
            config.write_text('''
#include "fixture.h"
Config load_config() { return {42}; }
Config& config() { static Config value = load_config(); return value; }
''', encoding='utf-8')
            logger = folder / 'logger.cc'
            logger.write_text('''
#include "fixture.h"
#include <cassert>
Logger logger(config());
int main() { assert(logger.value == 42); }
''', encoding='utf-8')
            binary = folder / 'example'
            for sources in ((config, logger), (logger, config)):
                with self.subTest(order=[p.name for p in sources]):
                    compiled = subprocess.run(
                        [COMPILER, '-std=c++23', '-Wall', '-Wextra', '-pedantic-errors',
                         *map(str, sources), '-o', str(binary)], capture_output=True,
                        text=True, timeout=40, check=False)
                    self.assertEqual(compiled.returncode, 0, compiled.stderr)
                    executed = subprocess.run([str(binary)], capture_output=True,
                                              text=True, timeout=5, check=False)
                    self.assertEqual(executed.returncode, 0, executed.stderr)
                    self.assertEqual(executed.stdout, '')

    def test_values_and_references_examples(self):
        bank = {q['id']: q for p in (ROOT / 'content/questions').rglob('*.json')
                for q in [json.loads(p.read_text(encoding='utf-8'))]}
        ledger = json.loads((ROOT / 'docs/reviews/references/2026-10-26-bindings.json').read_text())
        examples = {record['id']: record['example'] for record in ledger['references']}
        with tempfile.TemporaryDirectory(prefix='cpp-recall-values-references-') as directory:
            source = Path(directory) / 'example.cc'
            binary = Path(directory) / 'example'

            def check(code, *, output='', compiles=True):
                source.write_text('#include <cassert>\n#include <type_traits>\n' + code, encoding='utf-8')
                result = subprocess.run(
                    [COMPILER, '-std=c++23', '-Wall', '-Wextra', '-pedantic-errors', str(source), '-o', str(binary)],
                    capture_output=True, text=True, timeout=40, check=False)
                if not compiles:
                    self.assertNotEqual(result.returncode, 0, 'invalid reference/value assignment compiled')
                    return
                self.assertEqual(result.returncode, 0, result.stderr)
                executed = subprocess.run([str(binary)], capture_output=True, text=True, timeout=5, check=False)
                self.assertEqual(executed.returncode, 0, executed.stderr)
                self.assertEqual(executed.stdout, output)

            basics = bank['value_category_name_and_sum']['code']
            check('int main() {\n' + basics + '\n'
                  'static_assert(std::is_same_v<decltype((count)), int&>);\n'
                  'static_assert(std::is_same_v<decltype(count + 2), int>); assert(total == 6);\n}')
            assignment = bank['value_category_reference_call_assignment']['code']
            # Each line must be evaluated on its own, exactly as the prompt says.
            good = '\n'.join(line for line in assignment.splitlines() if '// A' not in line)
            good = good.replace('Second(settings) = 14;',
                                'Second(settings) = 14; assert(settings.limit == 14); '
                                'static_assert(std::is_same_v<decltype(Second(settings)), int&>);')
            check(good)
            bad = '\n'.join(line for line in assignment.splitlines() if '// Б' not in line)
            check(bad, compiles=False)
            const_access = bank['const_reference_observes_changes']['code']
            check(const_access.replace('limit += 4;', 'shown += 4;'), compiles=False)
            completion = bank['reference_return_existing_member']
            for answer in completion['task']['accepted_answers']:
                with self.subTest(answer=answer):
                    completed = completion['code'].replace('/* тип */', answer)
                    check(completed, output='3 12')
                    declarations = completed.split('int main()', 1)[0]
                    check(declarations + '''
int main() {
  Reading low{2}, high{17};
  assert(&Select(low, high, true) == &low.value);
  assert(&Select(low, high, false) == &high.value);
  Select(low, high, true) = 23;
  Select(low, high, false) = 31;
  assert(low.value == 23 && high.value == 31);
  Select(low, low, false) = 42;
  assert(low.value == 42);
}
''')
            for reference_id, assertion in {
                'value_categories_basics': 'assert(wider == 12);',
                'reference_alias_basics': 'assert(length == 13 && &left == &right && &right == &length);',
                'reference_function_access': 'assert(debt == 0);',
            }.items():
                with self.subTest(reference_id=reference_id):
                    code = examples[reference_id]
                    boundary = code.rfind('}')
                    check(code[:boundary] + assertion + '\n' + code[boundary:])

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
                grouped = {
                    'fold_validate_each_measurement': ['((... && (in_range((values)))))',
                                                       'return (((in_range)(values)) && ...);'],
                    'binary_fold_registration_fee': ['(((base) + ... + (extras)))',
                                                     'return ((extras) + ... + (base));'],
                }
                for answer in card['task']['accepted_answers'] + grouped[question_id]:
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

    def test_sfinae_accepted_conditions(self):
        card = json.loads((ROOT / 'content/questions/templates/sfinae_enable_if_integral_condition.json').read_text())
        checks = '''
template <typename T> concept CanEcho = requires(T value) { integral_echo(value); };
struct UserType {};
static_assert(CanEcho<bool> && CanEcho<char> && CanEcho<int> && CanEcho<unsigned>);
static_assert(!CanEcho<double> && !CanEcho<UserType> && !CanEcho<int*>);
static_assert(std::is_same_v<decltype(integral_echo(true)), bool>);
int main() { assert(integral_echo(17) == 17); assert(integral_echo(true)); }
'''
        with tempfile.TemporaryDirectory(prefix='cpp-recall-enable-if-') as directory:
            source = Path(directory) / 'condition.cc'
            binary = Path(directory) / 'condition'
            for answer in card['task']['accepted_answers']:
                with self.subTest(answer=answer):
                    code = card['code'].replace('/* допиши условие */', answer)
                    source.write_text('#include <cassert>\n' + code + '\n' + checks, encoding='utf-8')
                    compiled = subprocess.run(
                        [COMPILER, '-std=c++23', '-Wall', '-Wextra', '-pedantic-errors', str(source), '-o', str(binary)],
                        capture_output=True, text=True, timeout=40, check=False)
                    self.assertEqual(compiled.returncode, 0, compiled.stderr)
                    executed = subprocess.run([str(binary)], capture_output=True, text=True, timeout=5, check=False)
                    self.assertEqual(executed.returncode, 0, executed.stderr)

    def test_sfinae_body_is_expected_compile_failure(self):
        card = json.loads((ROOT / 'content/questions/templates/sfinae_body_error_is_hard_error.json').read_text())
        with tempfile.TemporaryDirectory(prefix='cpp-recall-sfinae-body-') as directory:
            source = Path(directory) / 'body.cc'
            source.write_text(card['code'], encoding='utf-8')
            result = subprocess.run(
                [COMPILER, '-std=c++23', '-Wall', '-Wextra', '-pedantic-errors', '-fsyntax-only', str(source)],
                capture_output=True, text=True, timeout=40, check=False)
            self.assertNotEqual(result.returncode, 0, 'body error unexpectedly selected the fallback')
            self.assertIn('value_type', result.stderr)

    def test_sfinae_reference_examples(self):
        ledger = json.loads((ROOT / 'docs/reviews/references/2026-10-26-bindings.json').read_text())
        examples = {r['id']: r['example'] for r in ledger['references']}
        cases = {
            'sfinae': '''
struct Tagged { using tag_type = long; };
static_assert(std::is_same_v<decltype(tag(static_cast<Tagged*>(nullptr))), long>);
static_assert(std::is_same_v<decltype(tag(static_cast<int*>(nullptr))), void>);
''',
            'enable_if': '''
template <typename T> concept CanCopy = requires(T value) { floating_copy(value); };
static_assert(CanCopy<float> && CanCopy<double>);
static_assert(!CanCopy<int> && !CanCopy<bool>);
''',
        }
        with tempfile.TemporaryDirectory(prefix='cpp-recall-sfinae-reference-') as directory:
            source = Path(directory) / 'reference.cc'
            for reference_id, checks in cases.items():
                with self.subTest(reference_id=reference_id):
                    source.write_text('#include <type_traits>\n' + examples[reference_id] + '\n' + checks, encoding='utf-8')
                    result = subprocess.run(
                        [COMPILER, '-std=c++23', '-Wall', '-Wextra', '-pedantic-errors', '-fsyntax-only', str(source)],
                        capture_output=True, text=True, timeout=40, check=False)
                    self.assertEqual(result.returncode, 0, result.stderr)

    def test_audit_distinguishing_examples(self):
        # Verify that plausible alternative mechanisms change the observation.
        cases = (
            ('references/reference_parameter_updates_argument', 'NextValue(int value)',
             'NextValue(int& value)', '11 11 13'),
            ('oop/oop_overload_then_virtual_dispatch', 'Base& base = object',
             'Derived& base = object', 'D-short'),
            ('containers/ranges_sort_member_projection', '&Row::score', '&Row::name', 'abc'),
            ('lifetime/placement_new_starts_new_lifetime', 'first->~Token();', '(void)first;', '02'),
            ('containers/map_update_existing_stock', 'stock.contains("tea")', 'true', 'out_of_range'),
        )
        for path, original, replacement, expected in cases:
            with self.subTest(path=path), tempfile.TemporaryDirectory(prefix='cpp-recall-audit-') as directory:
                card = json.loads((ROOT / 'content/questions' / (path + '.json')).read_text())
                self.assertEqual(card['code'].count(original), 1)
                code = card['code'].replace(original, replacement)
                if expected == 'out_of_range':
                    # Catch the specified exception rather than relying on an abort.
                    code = code.replace('  Update(b);', '  try { Update(b); } catch (const std::out_of_range&) { std::cout << "out_of_range"; }')
                    code = '#include <stdexcept>\n' + code
                    expected = '3 1 out_of_range'
                source = Path(directory) / 'example.cc'
                binary = Path(directory) / 'example'
                source.write_text(code, encoding='utf-8')
                compiled = subprocess.run([COMPILER, '-std=c++23', '-Wall', '-Wextra', '-pedantic-errors',
                                           str(source), '-o', str(binary)], capture_output=True, text=True, timeout=30)
                self.assertEqual(compiled.returncode, 0, compiled.stderr)
                run = subprocess.run([str(binary)], capture_output=True, text=True, timeout=5)
                self.assertEqual((run.returncode, run.stdout), (0, expected))

    def test_qa15_17_distinguishing_examples_and_help(self):
        card_root = ROOT / 'content/questions/templates'
        card = json.loads((card_root / 'decltype_auto_parenthesized_return.json').read_text())
        option = next(o for o in card['task']['options'] if o['id'] == card['task']['correct_option_id'])
        self.assertEqual(option['text'], '21')
        self.assertIn('auto copy()', card['code'])
        mutant = card['code'].replace('auto copy()', 'decltype(auto) copy()')
        with tempfile.TemporaryDirectory(prefix='cpp-recall-deduction-mutant-') as directory:
            source = Path(directory) / 'mutant.cc'
            binary = Path(directory) / 'mutant'
            source.write_text(mutant, encoding='utf-8')
            compiled = subprocess.run([COMPILER, '-std=c++23', '-Wall', '-Wextra', '-pedantic-errors',
                                       str(source), '-o', str(binary)], capture_output=True, text=True, timeout=30)
            self.assertEqual(compiled.returncode, 0, compiled.stderr)
            result = subprocess.run([str(binary)], capture_output=True, text=True, timeout=5)
            self.assertEqual((result.returncode, result.stdout), (0, '22'))
        requires = json.loads((card_root / 'requires_expression_invalid_requirement.json').read_text())
        compiled = self.compile_preparation(requires['code'])
        self.assertEqual(compiled.returncode, 0, compiled.stderr)
        material = json.loads((ROOT / 'content/cpp/topics/templates/skills/templates.sfinae/materials.json').read_text())
        refs = {r['id']: r for r in material['references']}
        bindings = {b['question_id']: b['reference_ids'] for b in material['reference_bindings']}
        for qid, hint, banned in (
            ('sfinae_enable_if_integral_condition', 'qa20261125.trait_selection', 'is_integral'),
            ('sfinae_body_error_is_hard_error', 'qa20261125.overload_phase_trace', 'не запускает выбор'),
        ):
            self.assertEqual(bindings[qid], [hint])
            article = refs[hint]
            self.assertNotIn(banned, ' '.join(article[k] for k in
                                               ('title', 'principle', 'mechanism', 'example', 'common_mistake')))
            compiled = self.compile_preparation(article['example'])
            self.assertEqual(compiled.returncode, 0, compiled.stderr)

    def test_qa20_static_constraints_and_slicing(self):
        root = ROOT / 'content/questions/oop'
        concept = json.loads((root / 'oop_concept_static_interface.json').read_text(encoding='utf-8'))
        constraints = concept['code'] + """
static_assert(Drawable<Circle>);
static_assert(!Drawable<Square>);
static_assert(!Drawable<Point>);
"""
        result = self.compile_preparation(constraints)
        self.assertEqual(result.returncode, 0, result.stderr)
        changed = constraints.replace('requires(const T& value)', 'requires(T& value)')
        self.assertNotEqual(self.compile_preparation(changed).returncode, 0)
        changed = changed.replace('static_assert(!Drawable<Square>);', 'static_assert(Drawable<Square>);')
        result = self.compile_preparation(changed)
        self.assertEqual(result.returncode, 0, result.stderr)
        interface = json.loads((root / 'oop_template_static_interface.json').read_text(encoding='utf-8'))
        valid = interface['code'] + '\nint main() { print_name(User{}); print_name(File{}); }'
        invalid = interface['code'] + '\nint main() { print_name(Task{}); }'
        self.assertNotEqual(self.compile_preparation(invalid).returncode, 0)
        slicing = json.loads((root / 'oop_vector_base_slices_derived.json').read_text(encoding='utf-8'))
        mutant = slicing['code'].replace('std::vector<Base>', 'std::vector<Derived>')
        cases = (
            (valid, 'userfile'),
            (mutant, 'DD'),
            (json.loads((root / 'oop_crtp_static_dispatch.json').read_text(encoding='utf-8'))['code']
             .replace('c.draw();\n  s.draw();', 's.draw();\n  c.draw();'), 'SC'),
        )
        with tempfile.TemporaryDirectory(prefix='cpp-recall-qa20-') as directory:
            source = Path(directory) / 'example.cc'
            binary = Path(directory) / 'example'
            for code, expected in cases:
                with self.subTest(expected=expected):
                    source.write_text(code, encoding='utf-8')
                    compiled = subprocess.run(
                        [COMPILER, '-std=c++23', '-Wall', '-Wextra', '-pedantic-errors',
                         str(source), '-o', str(binary)], capture_output=True, text=True, timeout=30)
                    self.assertEqual(compiled.returncode, 0, compiled.stderr)
                    result = subprocess.run([str(binary)], capture_output=True, text=True, timeout=5)
                    self.assertEqual((result.returncode, result.stdout), (0, expected))

    def test_qa22_destructor_exception_and_include_guard(self):
        root = ROOT / 'content/questions/language_core'
        card = json.loads((root / 'throwing_destructor_during_unwinding.json').read_text(encoding='utf-8'))
        code = card['code'].replace('#include <iostream>', '#include <iostream>\n#include <exception>\n#include <cstdlib>')
        code = code.replace('int main() {', 'int main() {\n  std::set_terminate([] { std::cout << "terminated" << std::flush; std::_Exit(0); });')
        ordinary = code.replace('    throw 1;', '')
        nonthrowing = ordinary.replace(' noexcept(false)', '')
        guard = json.loads((root / 'header_guard_purpose.json').read_text(encoding='utf-8'))['code']
        self.assertIn('// declarations', guard)
        with tempfile.TemporaryDirectory(prefix='cpp-recall-qa22-') as directory:
            source = Path(directory) / 'example.cc'
            binary = Path(directory) / 'example'
            header = Path(directory) / 'guard.h'
            header.write_text(guard.replace('// declarations', 'struct Guarded {};'), encoding='utf-8')
            source.write_text('#include "guard.h"\n#include "guard.h"\nint main() { Guarded value; (void)value; }\n',
                              encoding='utf-8')
            compiled = subprocess.run(
                [COMPILER, '-std=c++23', '-Wall', '-Wextra', '-pedantic-errors', str(source), '-o', str(binary)],
                capture_output=True, text=True, timeout=30)
            self.assertEqual(compiled.returncode, 0, compiled.stderr)
            header.write_text('struct Guarded {};\n', encoding='utf-8')
            duplicate = subprocess.run([COMPILER, '-std=c++23', '-fsyntax-only', str(source)],
                                       capture_output=True, text=True, timeout=30)
            self.assertNotEqual(duplicate.returncode, 0)
            for example, expected in ((code, 'terminated'), (ordinary, 'caught'), (nonthrowing, 'terminated')):
                with self.subTest(expected=expected):
                    source.write_text(example, encoding='utf-8')
                    compiled = subprocess.run(
                        [COMPILER, '-std=c++23', '-Wall', '-Wextra', '-pedantic-errors', str(source), '-o', str(binary)],
                        capture_output=True, text=True, timeout=30)
                    self.assertEqual(compiled.returncode, 0, compiled.stderr)
                    result = subprocess.run([str(binary)], capture_output=True, text=True, timeout=5)
                    self.assertEqual((result.returncode, result.stdout), (0, expected))

    def test_actual_card_programs(self):
        # Expected results are independently recorded by the source review, not read from the answer key.
        cases = {
            'atomic_fetch_add_returns_old_value': '57',
            'reference_alias_and_copy': '8 5',
            'reference_parameter_updates_argument': '9 11 11',
            'const_reference_observes_changes': '10',
            'compare_exchange_updates_expected': '022',
            'deferred_future_wait_for_status': '1',
            'shared_future_deferred_side_effect': '771',
            'decltype_auto_parenthesized_return': '21',
            'template_by_value_drops_top_const': '0',
            'templates_implicit_instantiation_pointer': '1',
            'unused_member_not_instantiated': 'ok',
            'shared_future_repeated_get': '77',
            'placement_new_starts_new_lifetime': '12',
            'invoke_member_data_write': '9',
            'invoke_const_member_function': '10',
            'lower_bound_first_equal_position': '1',
            'lower_bound_descending_boundary': '60',
            'span_observes_owner_changes': '18',
            'const_span_mutable_elements': '17',
            'comparator_equivalence_not_equality': '0 0 1',
            'ranges_sort_member_projection': 'cba',
            'parameter_pack_element_count': '3:3',
            'parameter_pack_expand_call': '7',
            'parameter_pack_frame_arguments': '4279',
            'sfinae_trailing_return_fallback': '3 -1',
            'fold_and_nonempty_pack': '10',
            'fold_subtraction_grouping': '12 16',
            'fold_and_empty_pack_identity': '110',
            'binary_fold_empty_pack_initial_value': '06',
            'vector_resize_adds_elements': '4 7',
            'find_missing_end_guard': 'нет',
            'vector_erase_return_next': '9 3',
            'vector_erase_filter_loop': '1 5 ',
            'map_find_without_insertion': '1 0 1',
            'map_insert_or_assign_vs_try_emplace': '2 4 1',
            'map_update_existing_stock': '3 1 8 2',
            'oop_overload_then_virtual_dispatch': 'D-int',
            'oop_crtp_static_dispatch': 'CS',
            'oop_vector_base_slices_derived': 'BD',
            'block_scope_destruction_order': 'BCA',
            'pointer_reference_share_object': '9 9',
            'unique_ptr_make_get_observe': '8 1',
            'unique_ptr_reset_destroys_now': 'AD1',
            'raii_unique_ptr_caught_exception': 'ARC',
            # The displayed program is defined: the question asks about a
            # hypothetical post-reset read. Never add that invalid read here.
            'unique_ptr_borrow_across_owner_move': '',
            'move_named_rvalue_reference_is_lvalue': 'CM',
            'move_if_noexcept_copy_fallback': 'C',
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
