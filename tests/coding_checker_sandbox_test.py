#!/usr/bin/env python3
"""Explicit opt-in local Docker QA. Authored sources execute ONLY through check.py.

Usage: python3 tests/coding_checker_sandbox_test.py --image sha256:<reviewed local ID>
No daemon setup, pulls or production access. Not part of ordinary Docker-free CI.
"""
import argparse
import hashlib
import json
from pathlib import Path
import signal
import subprocess
import sys
import tempfile
import time
import unittest
import uuid

ROOT = Path(__file__).resolve().parents[1]
IMAGE = None
ARTIFACTS = None
CORRECT = """std::vector<int> TakePrefix(std::vector<int>& pending, std::size_t limit) {
  auto n = std::min(limit, pending.size());
  std::vector<int> result(pending.begin(), pending.begin() + n);
  pending.erase(pending.begin(), pending.begin() + n);
  return result;
}
"""


class SandboxTest(unittest.TestCase):
    def submission(self, source):
        return dict(schema="cpp-recall-local-submission/v1", exercise_id="take_prefix", exercise_revision=1,
                    suite_id="take_prefix-public-v1", source=source,
                    source_sha256=hashlib.sha256(source.encode()).hexdigest(), attempt_id=str(uuid.uuid4()),
                    assistance={"help_opened": False, "solution_opened": False})

    def check_source(self, source, label):
        submission = ARTIFACTS / f"{label}-submission.json"
        report_path = ARTIFACTS / f"{label}-report.json"
        submission.write_text(json.dumps(self.submission(source)), encoding="utf-8")
        result = subprocess.run([sys.executable, str(ROOT / "tools/coding-checker/check.py"), str(submission),
                                 "--report", str(report_path), "--image", IMAGE],
                                capture_output=True, text=True, timeout=65)
        self.assertIn(result.returncode, (0, 1), result.stderr)
        report = json.loads(report_path.read_text())
        self.assertEqual(report["trust"], "local_unverified")
        self.assertEqual(report["source_sha256"], hashlib.sha256(source.encode()).hexdigest())
        return report

    def test_three_distinct_correct_implementations(self):
        alternatives = [CORRECT,
            """std::vector<int> TakePrefix(std::vector<int>& p, std::size_t n) {
              std::vector<int> a,b;
              for(std::size_t i=0;i<p.size();++i) (i<n?a:b).push_back(p[i]);
              p.swap(b); return a;
            }""",
            """std::vector<int> TakePrefix(std::vector<int>& p, std::size_t n) {
              std::vector<int> a;
              while(n && !p.empty()) { a.push_back(p.front()); p.erase(p.begin()); --n; }
              return a;
            }"""]
        for index, source in enumerate(alternatives):
            with self.subTest(index=index):
                report = self.check_source(source, f"correct-{index}")
                self.assertEqual(report["status"], "tests_passed", report)
                self.assertEqual(len(report["tests"]), 17)

    def test_wrong_remainder_has_observed_vectors(self):
        source = CORRECT.replace("pending.erase(pending.begin(), pending.begin() + n);", "")
        report = self.check_source(source, "wrong-remainder")
        self.assertEqual(report["status"], "tests_failed", report)
        row = next(row for row in report["tests"] if row["id"] == "partial_prefix")
        self.assertEqual(row["input_pending"], [4, 7, 4, 9])
        self.assertEqual(row["actual"], {"result": [4, 7], "pending": [4, 7, 4, 9]})

    def test_compile_error_has_user_coordinates(self):
        report = self.check_source("std::vector<int> TakePrefix( {", "compile-error")
        self.assertEqual(report["status"], "compile_error", report)
        self.assertIn("user_function.cpp", report["compile"]["diagnostics"])
        self.assertEqual(report["tests"], [])

    def test_program_stdout_is_not_a_verdict(self):
        source = CORRECT.replace("auto n =", 'std::cout << "student output is not JSON"; auto n =')
        report = self.check_source(source, "stdout")
        self.assertEqual(report["status"], "tests_passed", report)

    def test_bounded_output(self):
        source = "std::vector<int> TakePrefix(std::vector<int>&, std::size_t) { for(;;) std::cout << 'x'; }"
        report = self.check_source(source, "output-flood")
        self.assertEqual(report["status"], "output_limit", report)

    def test_bounded_time(self):
        source = "std::vector<int> TakePrefix(std::vector<int>&, std::size_t) { volatile int x=0; for(;;) { x=1; } }"
        report = self.check_source(source, "timeout")
        self.assertEqual(report["status"], "timeout", report)

    def test_bounded_memory(self):
        source = "std::vector<int> TakePrefix(std::vector<int>&, std::size_t) { std::vector<int> data(200000000); return data; }"
        report = self.check_source(source, "memory")
        self.assertEqual(report["status"], "resource_limit", report)

    def test_containers_are_cleaned_up(self):
        result = subprocess.run(["docker", "--host", "unix:///var/run/docker.sock", "ps", "-aq", "--filter",
                                 "name=cpp-recall-local-"], capture_output=True, text=True, check=True)
        self.assertEqual(result.stdout.strip(), "", "attempt containers remain after checker completion")

    def test_cancellation_cleans_up(self):
        source = "std::vector<int> TakePrefix(std::vector<int>&, std::size_t) { volatile int x=0; for(;;) { x=1; } }"
        submission = ARTIFACTS / "cancel-submission.json"
        submission.write_text(json.dumps(self.submission(source)), encoding="utf-8")
        process = subprocess.Popen([sys.executable, str(ROOT / "tools/coding-checker/check.py"), str(submission),
                                    "--report", str(ARTIFACTS / "cancel-report.json"), "--image", IMAGE],
                                   stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        try:
            found = False
            deadline = time.monotonic() + 8
            while time.monotonic() < deadline:
                listed = subprocess.run(["docker", "--host", "unix:///var/run/docker.sock", "ps", "-q",
                                         "--filter", "name=cpp-recall-local-"], capture_output=True, text=True, check=True)
                if listed.stdout.strip():
                    found = True
                    break
                time.sleep(0.1)
            self.assertTrue(found, "cancellation probe never reached a running container")
            process.send_signal(signal.SIGTERM)
            process.communicate(timeout=15)
            self.assertEqual(process.returncode, 130)
            self.test_containers_are_cleaned_up()
        finally:
            if process.poll() is None:
                process.send_signal(signal.SIGTERM)
                process.communicate(timeout=15)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--image", required=True)
    parser.add_argument("--artifacts", type=Path)
    args = parser.parse_args()
    IMAGE = args.image
    ARTIFACTS = args.artifacts or Path(tempfile.mkdtemp(prefix="cpp-recall-sandbox-qa-"))
    ARTIFACTS.mkdir(parents=True, exist_ok=True)
    print("Synthetic QA artifacts:", ARTIFACTS, flush=True)
    unittest.main(argv=[sys.argv[0]], verbosity=2)
