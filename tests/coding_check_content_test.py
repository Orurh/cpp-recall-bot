#!/usr/bin/env python3
"""CF-02 public cases: validate data and execute only fixed authored QA code.

No drafts or imported reports are loaded; this is not a user-code runner.
N4950 [vector.cons]/9 constructs the selected half-open range;
[sequence.reqmts]/51 removes that range. Empty [i,i) and full [begin,end)
ranges are valid when the iterators belong to the same valid sequence;
[iterator.requirements.general]/7-12 does not permit dereferencing end or using
invalid ranges. Passing these finite tests does not prove absence of UB.

Usage: python3 tests/coding_check_content_test.py [compiler]
"""

import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[1]
MANIFEST = ROOT / "docs/miniapp/coding-check-cases.json"
COMPILER = sys.argv[1] if len(sys.argv) > 1 else "g++-14"

# Only these authored constants and the safe mutations below become C++ source.
RANGE_SOLUTION = r"""
std::vector<int> TakePrefix(std::vector<int>& pending, std::size_t limit) {
  auto split = pending.begin();
  std::size_t count = 0;
  while (count < limit && split != pending.end()) {
    ++split;
    ++count;
  }
  std::vector<int> result(pending.begin(), split);
  pending.erase(pending.begin(), split);
  return result;
}
"""

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

SHIFT_SOLUTION = r"""
std::vector<int> TakePrefix(std::vector<int>& pending, std::size_t limit) {
  const auto size = pending.size();
  const auto count = limit < size ? limit : size;
  std::vector<int> result;
  for (std::size_t i = 0; i < count; ++i) result.push_back(pending[i]);
  for (std::size_t i = count; i < size; ++i) pending[i - count] = pending[i];
  pending.resize(size - count);
  return result;
}
"""

HEADERS = """#include <algorithm>
#include <cstddef>
#include <iostream>
#include <limits>
#include <vector>
"""


def vector_literal(values):
    # JSON strings are never inserted into generated source. Values are checked
    # as signed 32-bit integers before this helper is called.
    return "std::vector<int>{" + ",".join(str(value) for value in values) + "}"


def validate_manifest(manifest):
    if set(manifest) != {"exercise_id", "exercise_revision", "suite_id", "cases"}:
        raise ValueError("Unexpected manifest schema")
    if (manifest["exercise_id"], manifest["exercise_revision"], manifest["suite_id"]) != (
            "take_prefix", 1, "take_prefix-public-v1"):
        raise ValueError("Unexpected exercise or suite")
    if not isinstance(manifest["cases"], list) or len(manifest["cases"]) != 17:
        raise ValueError("Expected exactly 17 public rows")
    ids = set()
    for case in manifest["cases"]:
        required = {"id", "title", "pending", "limit", "expected"}
        if set(case) not in (required, required | {"sequence"}):
            raise ValueError("Unexpected case schema")
        if not isinstance(case["id"], str) or not case["id"].replace("_", "").isalnum():
            raise ValueError("Invalid case ID")
        if case["id"] in ids:
            raise ValueError("Duplicate case ID")
        ids.add(case["id"])
        if not isinstance(case["title"], str) or not case["title"].strip():
            raise ValueError("Missing public title")
        limit = case["limit"]
        if limit != "max" and not (type(limit) is int and 0 <= limit <= 9007199254740991):
            raise ValueError("Expected safe nonnegative integer or max")
        if not isinstance(case["expected"], dict) or set(case["expected"]) != {"result", "pending"}:
            raise ValueError("Unexpected expected schema")
        for values in (case["pending"], case["expected"]["result"], case["expected"]["pending"]):
            if not isinstance(values, list) or any(
                    type(value) is not int or not -2147483648 <= value <= 2147483647
                    for value in values):
                raise ValueError("Expected signed 32-bit integer arrays")
        if "sequence" in case and case["sequence"] != "repeated_calls":
            raise ValueError("Unexpected sequence")


def trusted_harness(cases):
    """Generate calls from validated data, reusing the actual sequence object."""
    statements = ["int main() {", "bool ok = true;"]
    started_sequence = False
    for index, case in enumerate(cases):
        sequence = case.get("sequence")
        pending_name = "sequence_pending" if sequence else f"pending_{index}"
        if not sequence or not started_sequence:
            statements.append(f"auto {pending_name} = {vector_literal(case['pending'])};")
        if sequence:
            started_sequence = True
        limit = ("std::numeric_limits<std::size_t>::max()" if case["limit"] == "max"
                 else f"std::size_t{{{case['limit']}}}")
        statements.append(f"const auto result_{index} = TakePrefix({pending_name}, {limit});")
        statements.append(
            f"if (result_{index} != {vector_literal(case['expected']['result'])} || "
            f"{pending_name} != {vector_literal(case['expected']['pending'])}) "
            f'{{ std::cerr << "FAIL {case["id"]}\\n"; ok = false; }}')
    statements.extend(['if (ok) std::cout << "PASS 17 public cases\\n";', "return ok ? 0 : 1;", "}"])
    return "\n".join(statements)


class CodingCheckContentTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.manifest = json.loads(MANIFEST.read_text(encoding="utf-8"))
        validate_manifest(cls.manifest)
        cls.cases = cls.manifest["cases"]

    def test_schema_and_expected_values_match_contract(self):
        validate_manifest(self.manifest)
        for case in self.cases:
            with self.subTest(case=case["id"]):
                pending = case["pending"]
                count = len(pending) if case["limit"] == "max" else min(len(pending), case["limit"])
                self.assertEqual(case["expected"], {"result": pending[:count], "pending": pending[count:]})
        self.assertEqual(sum("sequence" not in case for case in self.cases), 13)

    def test_repeated_calls_form_one_contiguous_sequence(self):
        sequence = [case for case in self.cases if "sequence" in case]
        self.assertEqual([case["id"] for case in sequence], [f"sequential_{i}" for i in range(1, 5)])
        self.assertEqual(self.cases[-4:], sequence)
        for previous, current in zip(sequence, sequence[1:]):
            self.assertEqual(current["pending"], previous["expected"]["pending"])
        self.assertEqual(sequence[-1]["expected"], {"result": [], "pending": []})

    def test_boundary_cases_are_explicit(self):
        cases = {case["id"]: case for case in self.cases}
        self.assertEqual(cases["empty_maximum"]["pending"], [])
        self.assertEqual(cases["empty_maximum"]["limit"], "max")
        self.assertEqual(cases["single_zero"]["limit"], 0)
        self.assertGreater(cases["single_oversized"]["limit"], len(cases["single_oversized"]["pending"]))
        self.assertEqual(cases["integer_extremes"]["pending"], [-2147483648, 0, 2147483647])

    def compile_and_run_trusted(self, code):
        source = HEADERS + code + trusted_harness(self.cases)
        with tempfile.TemporaryDirectory(prefix="cpp-recall-cf02-content-") as temporary:
            binary = Path(temporary) / "trusted-check"
            compiled = subprocess.run(
                [COMPILER, "-std=c++23", "-Wall", "-Wextra", "-Wpedantic", "-O0",
                 "-x", "c++", "-", "-o", str(binary)],
                input=source, text=True, capture_output=True, timeout=30,
            )
            self.assertEqual(compiled.returncode, 0, compiled.stderr)
            return subprocess.run([str(binary)], text=True, capture_output=True, timeout=5)

    def test_three_different_correct_trusted_implementations(self):
        for name, code in (("range_erase", RANGE_SOLUTION), ("rebuild", REBUILD_SOLUTION),
                           ("shift_resize", SHIFT_SOLUTION)):
            with self.subTest(implementation=name):
                result = self.compile_and_run_trusted(code)
                self.assertEqual(result.returncode, 0, result.stderr)
                self.assertIn("PASS 17 public cases", result.stdout)

    def test_five_safe_wrong_trusted_implementations_are_rejected(self):
        negatives = {
            "wrong_exact_boundary": (RANGE_SOLUTION.replace(
                "  auto split", "  if (limit >= pending.size()) return {};\n  auto split", 1), "exact_size"),
            "wrong_maximum": (RANGE_SOLUTION.replace(
                "  auto split", "  if (limit == std::numeric_limits<std::size_t>::max()) return {};\n  auto split", 1), "maximum_limit"),
            "reversed_result": (RANGE_SOLUTION.replace(
                "  return result;", "  std::reverse(result.begin(), result.end());\n  return result;"), "partial_prefix"),
            "lost_duplicates": (RANGE_SOLUTION.replace(
                "  return result;", "  result.erase(std::unique(result.begin(), result.end()), result.end());\n  return result;"), "duplicates_order"),
            "unchanged_source": (RANGE_SOLUTION.replace(
                "  pending.erase(pending.begin(), split);", ""), "partial_prefix"),
        }
        for name, (code, case_id) in negatives.items():
            with self.subTest(defect=name):
                result = self.compile_and_run_trusted(code)
                self.assertEqual(result.returncode, 1, result.stderr)
                self.assertIn(f"FAIL {case_id}", result.stderr)
                self.assertNotIn("PASS", result.stdout)


if __name__ == "__main__":
    unittest.main(argv=[sys.argv[0]], verbosity=2)
