#!/usr/bin/env python3
"""Contract and launcher tests: no daemon, no arbitrary host compilation."""

import copy
import hashlib
import importlib.util
import json
from pathlib import Path
import sys
import unittest
from unittest import mock

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location("coding_checker", ROOT / "tools/coding-checker/check.py")
checker = importlib.util.module_from_spec(spec)
spec.loader.exec_module(checker)
worker_spec = importlib.util.spec_from_file_location("coding_worker", ROOT / "tools/coding-checker/worker.py")
worker = importlib.util.module_from_spec(worker_spec)
worker_spec.loader.exec_module(worker)


def submission():
    source = "// exact text\n"
    return dict(schema="cpp-recall-local-submission/v1", exercise_id="take_prefix", exercise_revision=1,
                suite_id="take_prefix-public-v1", source=source,
                source_sha256=hashlib.sha256(source.encode()).hexdigest(),
                attempt_id="af67e445-2dfa-4e98-ae70-b38c147c9cd9",
                assistance={"help_opened": False, "solution_opened": False})


class CheckerTest(unittest.TestCase):
    def test_exact_source_binding_and_limits(self):
        value = submission()
        self.assertEqual(checker.validate_submission(value), value)
        for modify in (lambda x: x.update(source="// changed"),
                       lambda x: x.update(source="я" * 8193),
                       lambda x: x.update(exercise_revision=True),
                       lambda x: x.update(attempt_id="not-a-uuid"),
                       lambda x: x.update(assistance={"help_opened": 0, "solution_opened": False})):
            invalid = copy.deepcopy(value)
            modify(invalid)
            with self.assertRaises((ValueError, UnicodeError)):
                checker.validate_submission(invalid)

    def test_isolation_arguments_and_no_host_mount(self):
        arguments = checker.run_arguments("cpp-recall-local-test", "sha256:" + "1" * 64)
        for required in ("--pull=never", "--network=none", "--read-only", "--cap-drop=ALL",
                         "--security-opt=no-new-privileges:true", "--pids-limit=32", "--memory=384m",
                         "--memory-swap=384m", "--cpus=1", "--log-driver=none", "65532:65532"):
            self.assertIn(required, arguments)
        self.assertFalse(any(arg in ("--privileged", "--volume", "-v", "--mount", "--device") for arg in arguments))
        self.assertNotIn("docker.sock", " ".join(arguments))
        work = next(arg for arg in arguments if arg.startswith("/work:"))
        self.assertIn("exec", work.split(":", 1)[1].split(","))
        temporary = next(arg for arg in arguments if arg.startswith("/tmp:"))
        self.assertIn("noexec", temporary.split(":", 1)[1].split(","))

    def test_missing_capability_fails_closed(self):
        valid = dict(OSType="linux", CgroupVersion="2", MemoryLimit=True, PidsLimit=True, SwapLimit=True,
                     CpuCfsPeriod=True, CpuCfsQuota=True,
                     SecurityOptions=["name=seccomp,profile=builtin", "name=cgroupns"], Warnings=[])
        checker.validate_daemon(valid)
        for key in ("MemoryLimit", "PidsLimit", "CpuCfsPeriod", "CpuCfsQuota", "SecurityOptions"):
            invalid = dict(valid, **{key: False if key != "SecurityOptions" else []})
            with self.assertRaises(checker.CheckError):
                checker.validate_daemon(invalid)

    def test_no_docker_means_report_without_execution(self):
        with mock.patch.object(checker.shutil, "which", return_value=None), mock.patch.object(checker, "bounded_process") as execution:
            report = checker.check(submission(), "sha256:" + "1" * 64, "/var/run/docker.sock")
        self.assertEqual(report["status"], "sandbox_unavailable")
        self.assertEqual(report["trust"], "local_unverified")
        self.assertEqual(report["compile"]["status"], "not_run")
        self.assertIsInstance(report["compiler"], dict)
        execution.assert_not_called()

    def test_report_completeness_and_recomputed_comparison(self):
        manifest = {"cases": [{"id": "one", "pending": [1], "expected": {"result": [1], "pending": []}}]}
        report = checker.empty_report(submission(), "tests_passed")
        report["compiler"] = dict(id="gcc", version="14.4.0", standard="c++23", flags=checker.FLAGS)
        report["compile"]["status"] = "passed"
        report["tests"] = [{"id": "one", "status": "passed", "input_pending": [1], "actual": {"result": [1], "pending": []}}]
        self.assertEqual(checker.validate_worker_report(report, submission(), manifest), report)
        for mutation in (lambda r: r.update(tests=[]),
                         lambda r: r["tests"][0]["actual"].update(result=[2]),
                         lambda r: r.update(source_sha256="0" * 64),
                         lambda r: r["tests"][0].update(input_pending=[2]),
                         lambda r: r.update(attempt_id="different"),
                         lambda r: r["compile"].update(status="failed")):
            broken = copy.deepcopy(report)
            mutation(broken)
            with self.assertRaises(checker.CheckError):
                checker.validate_worker_report(broken, submission(), manifest)

    def test_mutated_sequence_uses_actual_previous_remainder(self):
        manifest = {"cases": [
            dict(id="one", pending=[1, 2], sequence="s", expected={"result": [1], "pending": [2]}),
            dict(id="two", pending=[2], sequence="s", expected={"result": [2], "pending": []})]}
        report = checker.empty_report(submission(), "tests_failed")
        report["compiler"] = dict(id="gcc", version="14.4.0", standard="c++23", flags=checker.FLAGS)
        report["compile"]["status"] = "passed"
        report["tests"] = [dict(id="one", input_pending=[1, 2], status="failed", actual={"result": [1], "pending": [1, 2]}),
                           dict(id="two", input_pending=[1, 2], status="failed", actual={"result": [1], "pending": [1, 2]})]
        checker.validate_worker_report(report, submission(), manifest)
        report["tests"][1]["input_pending"] = [2]
        with self.assertRaises(checker.CheckError):
            checker.validate_worker_report(report, submission(), manifest)

    def test_timeout_and_cancellation_target_only_attempt_container(self):
        info = dict(OSType="linux", CgroupVersion="2", MemoryLimit=True, PidsLimit=True, SwapLimit=True,
                    CpuCfsPeriod=True, CpuCfsQuota=True,
                    SecurityOptions=["name=seccomp,profile=builtin", "name=cgroupns"])
        image = "sha256:" + "1" * 64
        for raised in (checker.CheckError("timeout", "limit"), KeyboardInterrupt()):
            with self.subTest(exception=type(raised).__name__), \
                    mock.patch.object(checker.shutil, "which", return_value="/usr/bin/docker"), \
                    mock.patch.object(checker, "docker_prefix", return_value=["docker", "--host", "unix:///local.sock"]), \
                    mock.patch.object(checker, "docker_json", side_effect=[info, [{"Id": image, "Config": {}}]]), \
                    mock.patch.object(checker, "bounded_process", side_effect=[raised, (0, b"", b"")]) as execution:
                if isinstance(raised, KeyboardInterrupt):
                    with self.assertRaises(KeyboardInterrupt):
                        checker.check(submission(), image, "/local.sock")
                else:
                    self.assertEqual(checker.check(submission(), image, "/local.sock")["status"], "timeout")
                run = execution.call_args_list[0].args[0]
                cleanup = execution.call_args_list[1].args[0]
                name = run[run.index("--name") + 1]
                self.assertRegex(name, r"^cpp-recall-local-[a-f0-9]{32}$")
                self.assertEqual(cleanup[-3:], ["rm", "--force", name])
                self.assertNotIn("DOCKER_HOST", execution.call_args_list[0].kwargs["env"])

    def test_transport_timeout_and_output_are_bounded(self):
        with self.assertRaises(checker.CheckError) as timeout:
            checker.bounded_process([sys.executable, "-c", "import time; time.sleep(2)"], timeout=0.1)
        self.assertEqual(timeout.exception.status, "timeout")
        with self.assertRaises(checker.CheckError) as output:
            checker.bounded_process([sys.executable, "-c", "print('x' * 10000)"], timeout=2, limit=100)
        self.assertEqual(output.exception.status, "output_limit")

    def test_worker_does_not_execute_outside_container(self):
        with mock.patch.object(worker.Path, "exists", return_value=False), mock.patch.object(worker, "bounded_run") as execution:
            report = worker.worker(submission())
        self.assertEqual(report["status"], "runner_error")
        execution.assert_not_called()

    def test_exec_permission_denial_is_a_runner_failure_without_raw_paths(self):
        with mock.patch.object(worker.subprocess, "Popen", side_effect=PermissionError(13, "denied", "/private/example")):
            with self.assertRaises(worker.RunnerLaunchError) as launch:
                worker.bounded_run(["/work/attempt"], 5)
        self.assertIn("permission denied", str(launch.exception))
        self.assertNotIn("/private", str(launch.exception))

    def test_driver_uses_actual_values_and_preserves_sequence(self):
        cases = [dict(id="first", pending=[1, 2], limit=1, sequence="s"),
                 dict(id="second", pending=[2], limit="max", sequence="s")]
        generated = worker.harness("// authored source", cases)
        self.assertEqual(generated.count("pending = std::vector<int>"), 1)
        self.assertIn('std::numeric_limits<std::size_t>::max()', generated)
        self.assertIn('#line 1 "user_function.cpp"', generated)
        self.assertIn('std::ofstream records("/work/case-results.json")', generated)
        self.assertNotIn("std::cout", generated)
        self.assertNotIn('tests_passed', generated)


if __name__ == "__main__":
    unittest.main(verbosity=2)
