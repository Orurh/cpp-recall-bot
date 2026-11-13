"""Run the pilot's actual card and every offered placement as C++23, offline."""
import hashlib
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
CARD = ROOT / 'content/questions/language_core/raii_state_rollback_commit.json'
PROTOCOL = ROOT / 'content/transfer_pilots/raii_state_restore.json'


class TransferPilotContentTest(unittest.TestCase):
    def test_reviewed_revisions_and_family_reach_the_real_cli(self):
        protocol = json.loads(PROTOCOL.read_text(encoding='utf-8'))
        preparation = json.loads((ROOT / 'content/questions/language_core/vector_raii_early_return.json')
                                 .read_text(encoding='utf-8'))
        control = json.loads(CARD.read_text(encoding='utf-8'))
        for name, card in [('preparation', preparation), ('control', control)]:
            digest = hashlib.sha256(json.dumps(card, ensure_ascii=False, sort_keys=True,
                                              separators=(',', ':')).encode()).hexdigest()
            self.assertEqual(protocol[name]['revision'], digest)
            self.assertEqual(protocol[name]['question_id'], card['id'])
            self.assertEqual(protocol[name]['family_id'], card['question_family_id'])
        self.assertEqual(control['question_family_id'], preparation['question_family_id'])
        document = dict(schema='cpp-recall-retrieval-input/v1', complete=True,
                        observed_until='2026-11-30T00:00:00Z', tracking_since=None, events=[])
        result = subprocess.run([sys.executable, str(ROOT / 'scripts/report_transfer_pilot.py')],
                                input=json.dumps(document), text=True, capture_output=True, timeout=10)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(json.loads(result.stdout)['control_answers'], 0)

    def test_only_offered_placement_c_satisfies_both_postconditions(self):
        code = json.loads(CARD.read_text(encoding='utf-8'))['code']
        placements = [
            ('a', '  limit = proposed;', '  guard.commit();\n  limit = proposed;'),
            ('b', '    return false;', '    guard.commit();\n    return false;'),
            ('c', '  return true;', '  guard.commit();\n  return true;'),
            ('d', '  return true;', '  return true;\n  guard.commit();'),
        ]
        harness = r'''
#include <limits>
int main() {
  for (int initial : {-10, 0, 10, 101}) {
    for (int proposed : {std::numeric_limits<int>::min(), -1, 0, 1, 50, 100, 101,
                        std::numeric_limits<int>::max()}) {
      int limit = initial;
      const bool result = try_limit(limit, proposed);
      const bool valid = proposed >= 1 && proposed <= 100;
      if (result != valid || limit != (valid ? proposed : initial)) return 1;
    }
  }
  int repeated = 20;
  if (!try_limit(repeated, 30) || try_limit(repeated, 0) || repeated != 30) return 1;
}
'''
        with tempfile.TemporaryDirectory(prefix='transfer-pilot-cpp-') as directory:
            for option, marker, replacement in placements:
                with self.subTest(option=option):
                    self.assertEqual(code.count(marker), 1)
                    source = Path(directory) / f'{option}.cc'
                    binary = Path(directory) / option
                    source.write_text('#include <initializer_list>\n' + code.replace(marker, replacement)
                                      + harness, encoding='utf-8')
                    compile_result = subprocess.run([sys.argv[1] if len(sys.argv) > 1 else 'c++',
                                                     '-std=c++23', '-Wall', '-Wextra', '-pedantic-errors',
                                                     str(source), '-o', str(binary)],
                                                    text=True, capture_output=True, timeout=20)
                    self.assertEqual(compile_result.returncode, 0, compile_result.stderr)
                    result = subprocess.run([str(binary)], capture_output=True, timeout=5)
                    self.assertEqual(result.returncode, 0 if option == 'c' else 1)


if __name__ == '__main__':
    unittest.main(argv=[sys.argv[0]])
