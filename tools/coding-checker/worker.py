#!/usr/bin/env python3
"""Container-only worker. Never invoke directly on a host with a submission."""

import hashlib
import json
import os
from pathlib import Path
import resource
import selectors
import signal
import subprocess
import sys
import time

FLAGS = ["-std=c++23", "-Wall", "-Wextra", "-Wpedantic", "-O0"]
MANIFEST = Path("/opt/checker/cases.json")


class LimitError(Exception):
    def __init__(self, status):
        self.status = status


class RunnerLaunchError(Exception):
    """A local sandbox failure, rather than a failure inside the student's program."""


def bounded_run(arguments, seconds, limit=65536):
    try:
        process = subprocess.Popen(arguments, stdin=subprocess.DEVNULL, stdout=subprocess.PIPE,
                                   stderr=subprocess.PIPE, start_new_session=True,
                                   env={"PATH": "/usr/local/bin:/usr/bin:/bin", "LANG": "C.UTF-8", "TMPDIR": "/tmp"})
    except PermissionError as error:
        if arguments == ["/work/attempt"]:
            raise RunnerLaunchError("Sandbox could not launch the compiled program: permission denied") from error
        raise
    streams = {"stdout": bytearray(), "stderr": bytearray()}
    selector = selectors.DefaultSelector()
    for name in streams:
        stream = getattr(process, name)
        os.set_blocking(stream.fileno(), False)
        selector.register(stream, selectors.EVENT_READ, name)
    deadline = time.monotonic() + seconds
    try:
        while selector.get_map():
            if time.monotonic() >= deadline:
                raise LimitError("timeout")
            for key, _ in selector.select(0.05):
                chunk = os.read(key.fd, 8192)
                if not chunk:
                    selector.unregister(key.fileobj)
                    continue
                streams[key.data].extend(chunk)
                if sum(map(len, streams.values())) > limit:
                    raise LimitError("output_limit")
        process.wait(timeout=max(0.01, deadline - time.monotonic()))
        return process.returncode, *(bytes(streams[name]) for name in ("stdout", "stderr"))
    except subprocess.TimeoutExpired as error:
        raise LimitError("timeout") from error
    finally:
        selector.close()
        if process.poll() is None:
            try:
                os.killpg(process.pid, signal.SIGKILL)
            except ProcessLookupError:
                pass
            process.wait()
        process.stdout.close()
        process.stderr.close()


def vector_literal(items):
    return "std::vector<int>{" + ",".join(str(item) for item in items) + "}"


def harness(source, cases):
    # Expected values stay in the Python comparator, not in the student process.
    result = """#include <algorithm>
#include <array>
#include <cstddef>
#include <fstream>
#include <iostream>
#include <limits>
#include <vector>
#line 1 "user_function.cpp"
""" + source + """
#line 1 "checker_driver.cpp"
namespace cf_driver {
std::ofstream records("/work/case-results.json");
void vector_json(const std::vector<int>& value) {
  records << '[';
  for (std::size_t i = 0; i < value.size(); ++i) {
    if (i) records << ',';
    records << value[i];
  }
  records << ']';
}
void run(std::vector<int>& pending, std::size_t limit, const char* id) {
  const auto input = pending;
  const auto result = TakePrefix(pending, limit);
  records << "{\\"id\\":\\"" << id << "\\",\\"input_pending\\":";
  vector_json(input);
  records << ",\\"actual\\":{\\"result\\":";
  vector_json(result);
  records << ",\\"pending\\":";
  vector_json(pending);
  records << "}}";
}
}
int main() {
  cf_driver::records << '[';
"""
    last_sequence = None
    for index, case in enumerate(cases):
        # All IDs are authored ASCII identifiers; no user source enters generated literals.
        if index:
            result += "  cf_driver::records << ',';\n"
        sequence = case.get("sequence")
        if sequence is None or sequence != last_sequence:
            result += "  " + ("auto " if index == 0 else "") + "pending = " + vector_literal(case["pending"]) + ";\n"
        limit = "std::numeric_limits<std::size_t>::max()" if case["limit"] == "max" else str(case["limit"])
        result += '  cf_driver::run(pending, ' + limit + ', "' + case["id"] + '");\n'
        last_sequence = sequence
    return result + "  cf_driver::records << ']';\n  cf_driver::records.close();\n  return cf_driver::records ? 0 : 2;\n}\n"


def worker(submission):
    identity = {key: submission[key] for key in (
        "exercise_id", "exercise_revision", "suite_id", "source_sha256", "attempt_id", "assistance")}
    report = dict(identity, schema="cpp-recall-local-check/v1", trust="local_unverified",
                  status="runner_error", compiler={"id": "unavailable", "version": "unknown",
                                                   "standard": "c++23", "flags": []},
                  compile={"status": "not_run", "diagnostics": ""}, tests=[])
    try:
        # The worker fails closed when accidentally invoked outside the reviewed container.
        limits = ((resource.RLIMIT_NPROC, 32), (resource.RLIMIT_NOFILE, 64),
                  (resource.RLIMIT_FSIZE, 16777216), (resource.RLIMIT_CPU, 35))
        if (not Path("/.dockerenv").exists() or os.getuid() != 65532 or Path.cwd() != Path("/work")
                or any(not 0 < resource.getrlimit(kind)[0] <= maximum for kind, maximum in limits)):
            raise ValueError("Container environment required")
        security = Path("/proc/self/status").read_text()
        if "NoNewPrivs:\t1" not in security or "CapEff:\t0000000000000000" not in security:
            raise ValueError("Restricted container credentials required")
        cgroup = Path("/sys/fs/cgroup")
        if (int((cgroup / "memory.max").read_text()) > 384 * 1024 * 1024
                or int((cgroup / "pids.max").read_text()) > 32
                or int((cgroup / "memory.swap.max").read_text()) != 0):
            raise ValueError("Effective cgroup limits required")
        quota, period = map(int, (cgroup / "cpu.max").read_text().split())
        if quota <= 0 or quota > period:
            raise ValueError("Effective CPU limit required")
        if (submission["schema"] != "cpp-recall-local-submission/v1"
                or submission["exercise_id"] != "take_prefix" or submission["exercise_revision"] != 1
                or submission["suite_id"] != "take_prefix-public-v1"
                or len(submission["source"].encode()) > 16384
                or hashlib.sha256(submission["source"].encode()).hexdigest() != submission["source_sha256"]):
            raise ValueError("Invalid submission")
        manifest = json.loads(MANIFEST.read_text())
        code, version, _ = bounded_run(["g++", "-dumpfullversion"], 2, 4096)
        if code or not version.decode().strip().startswith("14."):
            raise ValueError("Reviewed GCC 14 compiler required")
        report["compiler"] = {"id": "gcc", "version": version.decode().strip()[:256],
                              "standard": "c++23", "flags": FLAGS}
        Path("/work/submission.cpp").write_text(harness(submission["source"], manifest["cases"]), encoding="utf-8")
        report["compile"]["status"] = "failed"
        code, _, diagnostics = bounded_run(["g++", *FLAGS, "/work/submission.cpp", "-o", "/work/attempt"], 30)
        report["compile"]["diagnostics"] = diagnostics.decode("utf-8", errors="replace")[:16384]
        if code:
            report["status"] = "resource_limit" if code < 0 else "compile_error"
            return report
        report["compile"]["status"] = "passed"
        code, output, diagnostics = bounded_run(["/work/attempt"], 5)
        if code:
            report["status"] = "resource_limit" if code in (-signal.SIGKILL, -signal.SIGXCPU, -signal.SIGXFSZ) else "runtime_error"
            report["compile"]["diagnostics"] += "\nRuntime: " + diagnostics.decode("utf-8", errors="replace")[:8192]
            report["compile"]["diagnostics"] = report["compile"]["diagnostics"][:16384]
            return report
        # Student stdout/stderr are capped separately; debug printing cannot imitate a result.
        # This local artifact has no cryptographic provenance guarantee.
        with Path("/work/case-results.json").open("rb") as records:
            result_bytes = records.read(65537)
        if len(result_bytes) > 65536:
            raise LimitError("output_limit")
        rows = json.loads(result_bytes)
        if not isinstance(rows, list) or len(rows) != len(manifest["cases"]):
            raise ValueError("Incomplete actual test output")
        for row, case in zip(rows, manifest["cases"]):
            if not isinstance(row, dict) or row.get("id") != case["id"] or not isinstance(row.get("actual"), dict):
                raise ValueError("Invalid actual test output")
            actual = row["actual"]
            if (set(actual) != {"result", "pending"}
                    or any(not isinstance(items, list) or len(items) > 1024
                           or any(type(item) is not int or not -(2**31) <= item < 2**31 for item in items)
                           for items in actual.values())):
                raise ValueError("Invalid actual vectors")
            report["tests"].append({"id": case["id"], "status": "passed" if actual == case["expected"] else "failed",
                                    "input_pending": row.get("input_pending"), "actual": actual})
        report["status"] = "tests_passed" if all(row["status"] == "passed" for row in report["tests"]) else "tests_failed"
    except RunnerLaunchError as error:
        report["status"] = "runner_error"
        report["tests"] = []
        report["compile"]["diagnostics"] = (report["compile"]["diagnostics"] + "\n" + str(error))[:16384]
    except LimitError as error:
        report["status"] = error.status
        report["tests"] = []
    except (ValueError, KeyError, TypeError, OSError, UnicodeError) as error:
        report["status"] = "runtime_error" if report["compile"]["status"] == "passed" else "runner_error"
        report["tests"] = []
        report["compile"]["diagnostics"] = (report["compile"]["diagnostics"] + "\nInvalid or incomplete checker output")[:16384]
    return report


def main():
    # Input is bounded before JSON decoding; Docker receives only this JSON over stdin.
    raw = sys.stdin.buffer.read(131073)
    if len(raw) > 131072:
        return 2
    try:
        submission = json.loads(raw)
        report = worker(submission)
        sys.stdout.write(json.dumps(report, ensure_ascii=False))
        return 0
    except (ValueError, KeyError, TypeError):
        return 2


if __name__ == "__main__":
    sys.exit(main())
