#!/usr/bin/env python3
"""CF-01: verify static content and compile only trusted authored QA constants.

No drafts, live input, network requests, eval, or product code execution service.
Usage: python3 tests/coding_practice_content_test.py [compiler] [node]
"""

import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[1]
MODULE = ROOT / "docs/miniapp/coding-practice.js"
REVIEW = ROOT / "docs/reviews/theory/2026-11-05-coding-prefix.json"
COMPILER = sys.argv[1] if len(sys.argv) > 1 else os.environ.get("CXX", "g++-14")
NODE = sys.argv[2] if len(sys.argv) > 2 else os.environ.get("NODE", "node")

# Fixed JavaScript loads our static module; source values are data, never evaluated.
NODE_LOADER = r"""
const fs = require('node:fs');
require(process.argv[1]);
const p = globalThis.CppRecallCodingPractice;
const request = JSON.parse(fs.readFileSync(0, 'utf8'));
if (request.mode === 'source') {
  process.stdout.write(p.buildSource(request.code));
} else {
  const frozen = value => !value || typeof value !== 'object' ||
    (Object.isFrozen(value) && Object.values(value).every(frozen));
  const marker = 'globalThis.__draftExecuted = true;';
  const combined = p.buildSource(marker);
  let overlongRejected = false;
  let nonStringRejected = false;
  let unicodeOverlongRejected = false;
  try { p.buildSource('x'.repeat(p.MAX_CODE_LENGTH + 1)); }
  catch (error) { overlongRejected = error instanceof RangeError; }
  try { p.buildSource(null); }
  catch (error) { nonStringRejected = error instanceof TypeError; }
  try { p.buildSource('я'.repeat(p.MAX_CODE_LENGTH / 2 + 1)); }
  catch (error) { unicodeOverlongRejected = error instanceof RangeError; }
  process.stdout.write(JSON.stringify({
    practice: p, frozen: frozen(p), overlongRejected, nonStringRejected,
    atLimitAccepted: p.buildSource('x'.repeat(p.MAX_CODE_LENGTH)).includes('x'.repeat(p.MAX_CODE_LENGTH)),
    unicodeAtLimitAccepted: p.buildSource('я'.repeat(p.MAX_CODE_LENGTH / 2)).includes('я'.repeat(p.MAX_CODE_LENGTH / 2)),
    unicodeOverlongRejected,
    markerPreserved: combined.includes(marker),
    draftExecuted: globalThis.__draftExecuted === true
  }));
}
"""

# Trusted independent solution: construct prefix and suffix independently, then swap.
REBUILD_SOLUTION = r"""
std::vector<int> TakePrefix(std::vector<int>& pending, std::size_t limit) {
  std::vector<int> result;
  std::vector<int> remainder;
  for (std::size_t i = 0; i < pending.size(); ++i) {
    if (i < limit) result.push_back(pending[i]);
    else remainder.push_back(pending[i]);
  }
  pending.swap(remainder);
  return result;
}
"""

# Trusted independent solution: copy the prefix, shift the suffix, then resize.
SHIFT_SOLUTION = r"""
std::vector<int> TakePrefix(std::vector<int>& pending, std::size_t limit) {
  const auto original_size = pending.size();
  const auto count = limit < original_size ? limit : original_size;
  std::vector<int> result;
  for (std::size_t i = 0; i < count; ++i) result.push_back(pending[i]);
  for (std::size_t i = count; i < original_size; ++i) {
    pending[i - count] = pending[i];
  }
  pending.resize(original_size - count);
  return result;
}
"""


def module_request(request):
    return subprocess.run(
        [NODE, "-e", NODE_LOADER, str(MODULE)],
        input=json.dumps(request), text=True, capture_output=True,
        check=True, timeout=10,
    ).stdout


class CodingPracticeContentTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.loaded = json.loads(module_request({"mode": "contract"}))
        cls.practice = cls.loaded["practice"]

    def test_contract_and_pure_source_export(self):
        p = self.practice
        self.assertEqual(p["id"], "take_prefix")
        self.assertEqual(p["revision"], 1)
        self.assertEqual(p["MAX_CODE_LENGTH"], 16384)
        self.assertTrue(self.loaded["frozen"])
        self.assertTrue(self.loaded["overlongRejected"])
        self.assertTrue(self.loaded["nonStringRejected"])
        self.assertTrue(self.loaded["atLimitAccepted"])
        self.assertTrue(self.loaded["unicodeAtLimitAccepted"])
        self.assertTrue(self.loaded["unicodeOverlongRejected"])
        self.assertTrue(self.loaded["markerPreserved"])
        self.assertFalse(self.loaded["draftExecuted"])
        self.assertIn("CF-01", p["title"])
        for field in ("title", "summary", "prompt", "signature", "starter", "solution", "testsSource"):
            self.assertIsInstance(p[field], str)
            self.assertTrue(p[field].strip())
        for field in ("prerequisites", "explanation", "rubric"):
            self.assertTrue(p[field])
            self.assertTrue(all(isinstance(item, str) and item.strip() for item in p[field]))
        for example in p["examples"]:
            self.assertEqual(set(example), {"input", "output", "remaining"})
        self.assertIn("код не запускает и не оценивает", p["prompt"])
        self.assertIn("XP и подтверждения навыка нет", p["prompt"])
        self.assertIn("без main()", p["prompt"])
        self.assertLess(len(p["prompt"].split()), 120)
        self.assertIn("g++ -std=c++23", p["compileCommand"])
        self.assertIn("./take_prefix", p["compileCommand"])
        self.assertNotIn("return", p["starter"])
        self.assertNotIn("pending.", p["starter"])
        self.assertIn(p["signature"], p["starter"])
        self.assertIn("первые limit элементов pending (если элементов меньше — все)", p["prompt"])
        self.assertIn("первые limit элементов pending (если их меньше — все)", p["summary"])
        self.assertIn("без main()", p["summary"])
        self.assertIn("ровно min(limit, исходный размер pending)", p["rubric"][1])
        self.assertNotIn("public_id", p)
        self.assertNotIn("question_family_id", p)
        source = MODULE.read_text(encoding="utf-8")
        for forbidden in ("eval(", "new Function", "fetch(", "XMLHttpRequest", "child_process", "import("):
            self.assertNotIn(forbidden, source)

    def test_help_is_a_distinct_copy_example_and_review_is_pinned(self):
        p = self.practice
        help_text = p["help"]
        self.assertIn("std::array", help_text["code"])
        self.assertIn("snapshot", help_text["code"])
        for leak in ("TakePrefix", "pending", "limit", "erase", "split"):
            self.assertNotIn(leak, help_text["code"])
        self.assertTrue(help_text["paragraphs"])
        self.assertTrue(help_text["sources"])
        for source in help_text["sources"]:
            self.assertTrue(source["title"])
            self.assertTrue(source["url"].startswith("https://timsong-cpp.github.io/cppwp/n4950/"))
        review = json.loads(REVIEW.read_text(encoding="utf-8"))
        self.assertEqual(review["reviewed_content_sha256"], hashlib.sha256(MODULE.read_bytes()).hexdigest())
        self.assertFalse(review["reviewer"]["independent"])
        self.assertEqual(review["exercise_id"], p["id"])
        self.assertEqual(review["exercise_revision"], p["revision"])
        self.assertEqual({item["public_id"] for item in review["independence_review"]},
                         {"CR-000030", "CR-000176", "CR-000177", "CR-000213"})

    def compile_source_trusted(self, source):
        # Only authored constants/reference are passed here, never user drafts.
        with tempfile.TemporaryDirectory(prefix="cpp-recall-cf-content-") as temporary:
            binary = Path(temporary) / "trusted-check"
            compiled = subprocess.run(
                [COMPILER, "-std=c++23", "-Wall", "-Wextra", "-Wpedantic", "-O0",
                 "-x", "c++", "-", "-o", str(binary)],
                input=source, text=True, capture_output=True, timeout=30,
            )
            self.assertEqual(compiled.returncode, 0, compiled.stderr)
            return subprocess.run([str(binary)], text=True, capture_output=True, timeout=5)

    def compile_and_run_trusted(self, code):
        return self.compile_source_trusted(module_request({"mode": "source", "code": code}))

    def test_exact_help_example_compiles_and_runs(self):
        result = self.compile_source_trusted(self.practice["help"]["code"])
        self.assertEqual(result.returncode, 0, result.stderr)

    def test_three_different_correct_trusted_implementations(self):
        for name, code in (("range_erase_reference", self.practice["solution"]),
                           ("rebuild_two_vectors", REBUILD_SOLUTION),
                           ("shift_and_resize", SHIFT_SOLUTION)):
            with self.subTest(implementation=name):
                result = self.compile_and_run_trusted(code)
                self.assertEqual(result.returncode, 0, result.stderr)
                self.assertIn("Открытые тесты пройдены", result.stdout)

    def test_public_harness_rejects_safe_wrong_trusted_implementations(self):
        reference = self.practice["solution"]
        negatives = {
            "exact_and_oversized_boundary": reference.replace(
                "  auto split", "  if (limit >= pending.size()) return {};\n  auto split", 1),
            "maximum_limit": reference.replace(
                "  auto split", "  if (limit == std::numeric_limits<std::size_t>::max()) return {};\n  auto split", 1),
            "order": reference.replace("  return result;", "  std::reverse(result.begin(), result.end());\n  return result;"),
            "duplicates": reference.replace("  return result;", "  result.erase(std::unique(result.begin(), result.end()), result.end());\n  return result;"),
            "missing_mutation": reference.replace("  pending.erase(pending.begin(), split);", ""),
        }
        for name, code in negatives.items():
            with self.subTest(defect=name):
                result = self.compile_and_run_trusted(code)
                self.assertNotEqual(result.returncode, 0)
                self.assertIn("Не выполнен случай", result.stderr)


if __name__ == "__main__":
    unittest.main(argv=[sys.argv[0]], verbosity=2)
