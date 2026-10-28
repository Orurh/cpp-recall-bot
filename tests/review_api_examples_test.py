"""Reproduce the reviewed contracts; compilation is not a substitute for N4950."""
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
COMPILER = sys.argv.pop(1) if len(sys.argv) > 1 else 'c++'


def load_card(name):
    return json.loads((ROOT / f'content/questions/containers/{name}.json').read_text(encoding='utf-8'))


class ReviewApiExamplesTest(unittest.TestCase):
    def check_program(self, code):
        with tempfile.TemporaryDirectory(prefix='cpp-recall-review-api-') as directory:
            source = Path(directory) / 'example.cc'
            binary = Path(directory) / 'example'
            source.write_text('#include <cassert>\n#include <type_traits>\n#include <stdexcept>\n' + code,
                              encoding='utf-8')
            compiled = subprocess.run(
                [COMPILER, '-std=c++23', '-Wall', '-Wextra', '-pedantic-errors', str(source), '-o', str(binary)],
                capture_output=True, text=True, timeout=40, check=False)
            self.assertEqual(compiled.returncode, 0, compiled.stderr)
            executed = subprocess.run([str(binary)], capture_output=True, text=True, timeout=5, check=False)
            self.assertEqual(executed.returncode, 0, executed.stderr)

    def test_written_method_calls(self):
        cases = {
            'map_find_iterator_completion': '''
  const Cache cache{{"a", 0}, {"z", 12}};
  const auto before = cache;
  assert(Locate(cache, "a")->second == 0);
  assert(Locate(cache, "z")->second == 12);
  assert(Locate(cache, "g") == cache.end());
  const Cache empty;
  assert(Locate(empty, "g") == empty.end());
  assert(cache == before);
  static_assert(std::is_same_v<decltype(Locate(cache, "a")), Cache::const_iterator>);
''',
            'map_contains_completion': '''
  const std::map<std::string, int> cache{{"a", 0}, {"z", -3}};
  const auto before = cache;
  assert(HasKey(cache, "a") && HasKey(cache, "z"));
  assert(!HasKey(cache, "g"));
  const std::map<std::string, int> empty;
  assert(!HasKey(empty, "a"));
  assert(cache == before);
  const std::string key = "a";
  static_assert(std::is_same_v<decltype(cache.contains(key)), bool>);
''',
            'map_try_emplace_completion': '''
  std::map<std::string, int> prices{{"zero", 0}, {"old", -3}};
  const auto before = prices;
  assert(!Register(prices, "zero", 7) && !Register(prices, "old", 9));
  assert(prices == before);
  assert(Register(prices, "new", 13));
  assert(!Register(prices, "new", 21));
  assert(prices.at("new") == 13 && prices.size() == 3);
  std::map<std::string, int> empty;
  assert(Register(empty, "first", 0) && empty.at("first") == 0);
''',
            'map_insert_or_assign_completion': '''
  std::map<std::string, int> prices{{"zero", 0}, {"old", -3}};
  assert(!Store(prices, "zero", 7) && prices.at("zero") == 7);
  assert(!Store(prices, "old", 9) && prices.at("old") == 9);
  assert(Store(prices, "new", 13));
  assert(!Store(prices, "new", 21));
  assert(!Store(prices, "new", 21));
  assert(prices.at("new") == 21 && prices.size() == 3);
  std::map<std::string, int> empty;
  assert(Store(empty, "first", 0) && empty.at("first") == 0);
''',
        }
        for question_id, checks in cases.items():
            card = load_card(question_id)
            for answer in card['task']['accepted_answers']:
                with self.subTest(question_id=question_id, answer=answer):
                    code = card['code'].replace('/* вызов метода */', answer)
                    self.check_program(code + '\nint main() {\n' + checks + '\n}\n')

    def test_review_verdicts_and_repair(self):
        bad = load_card('review_cache_read_contract')['code']
        good = load_card('review_mark_existing_contract')['code']
        repaired = bad.replace('int Read(', 'int ReadFixed(').replace(
            'return cache[key];',
            'const auto it = cache.find(key); return it == cache.end() ? 0 : it->second;')
        wrong_repair = bad.replace('int Read(', 'int ReadAt(').replace('return cache[key];', 'return cache.at(key);')
        self.check_program(bad + '\n' + good + '\n' + repaired + '\n' + wrong_repair + '''
int main() {
  std::map<std::string, int> cache{{"zero", 0}, {"old", 9}};
  const auto before = cache;
  assert(Read(cache, "zero") == 0 && Read(cache, "old") == 9);
  assert(cache == before);
  assert(Read(cache, "missing") == 0);
  assert(cache.size() == before.size() + 1 && cache.contains("missing"));
  std::map<std::string, int> empty;
  assert(Read(empty, "new") == 0 && empty.size() == 1);
  cache = before;
  assert(ReadFixed(cache, "zero") == 0 && ReadFixed(cache, "old") == 9);
  assert(ReadFixed(cache, "missing") == 0 && cache == before);
  empty.clear();
  assert(ReadFixed(empty, "missing") == 0 && empty.empty());
  bool threw = false;
  try { (void)ReadAt(empty, "missing"); }
  catch (const std::out_of_range&) { threw = true; }
  assert(threw && empty.empty());

  std::map<std::string, bool> seen{{"new", false}, {"old", true}, {"other", false}};
  const auto original = seen;
  assert(!MarkSeen(seen, "missing") && seen == original);
  assert(MarkSeen(seen, "new") && seen.at("new"));
  assert(MarkSeen(seen, "new") && MarkSeen(seen, "old"));
  assert(!seen.at("other") && seen.size() == original.size());
  std::map<std::string, bool> none;
  assert(!MarkSeen(none, "missing") && none.empty());
  const auto it = seen.find("new");
  static_assert(std::is_same_v<decltype((it->second)), bool&>);
  const auto& readonly = seen;
  const auto const_it = readonly.find("new");
  static_assert(std::is_same_v<decltype((const_it->second)), const bool&>);
}
''')

    def test_exact_contract_reading_reference(self):
        ledger = json.loads((ROOT / 'docs/reviews/references/2026-10-26-bindings.json').read_text())
        example = next(r['example'] for r in ledger['references'] if r['id'] == 'library_contract_reading')
        self.check_program('#include <string>\nint main() {\n' + example + '''
  assert(word == "planet" && part == "ane");
  assert(word.substr(word.size()).empty());
  assert(word.substr(2, 100) == "anet");
  bool threw = false;
  try { (void)word.substr(word.size() + 1); }
  catch (const std::out_of_range&) { threw = true; }
  assert(threw && word == "planet");
}
''')


if __name__ == '__main__':
    unittest.main()
