#!/usr/bin/env python3
"""Explicit, offline Docker launcher. Never compiles or runs submissions on the host."""

import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import selectors
import shutil
import signal
import stat
import subprocess
import sys
import tempfile
import time
import uuid

MAX_SOURCE = 16384
MAX_OUTPUT = 131072
FLAGS = ["-std=c++23", "-Wall", "-Wextra", "-Wpedantic", "-O0"]
STATUSES = {"tests_passed", "tests_failed", "compile_error", "runtime_error", "timeout",
            "resource_limit", "output_limit", "sandbox_unavailable", "runner_error"}
MANIFEST = Path(__file__).resolve().parents[2] / "docs/miniapp/coding-check-cases.json"


class CheckError(Exception):
    def __init__(self, status, message):
        self.status = status
        super().__init__(message)


def validate_submission(value):
    if not isinstance(value, dict):
        raise ValueError("Submission must be an object")
    if (value.get("schema") != "cpp-recall-local-submission/v1"
            or value.get("exercise_id") != "take_prefix"
            or type(value.get("exercise_revision")) is not int
            or value["exercise_revision"] != 1
            or value.get("suite_id") != "take_prefix-public-v1"):
        raise ValueError("Unsupported submission identity")
    source = value.get("source")
    if not isinstance(source, str) or len(source.encode("utf-8")) > MAX_SOURCE:
        raise ValueError("Source must be UTF-8, at most 16384 bytes")
    if value.get("source_sha256") != hashlib.sha256(source.encode("utf-8")).hexdigest():
        raise ValueError("Source hash mismatch")
    attempt = value.get("attempt_id")
    if not isinstance(attempt, str) or str(uuid.UUID(attempt)) != attempt:
        raise ValueError("Attempt ID must be a canonical UUID")
    assistance = value.get("assistance")
    if (not isinstance(assistance, dict)
            or set(assistance) != {"help_opened", "solution_opened"}
            or any(type(v) is not bool for v in assistance.values())):
        raise ValueError("Invalid assistance metadata")
    return value


def empty_report(submission, status="runner_error", diagnostics=""):
    identity = {key: submission[key] for key in (
        "exercise_id", "exercise_revision", "suite_id", "source_sha256", "attempt_id", "assistance")}
    return dict(identity, schema="cpp-recall-local-check/v1", trust="local_unverified",
                status=status, compiler={"id": "unavailable", "version": "unknown",
                                         "standard": "c++23", "flags": []},
                compile={"status": "not_run", "diagnostics": diagnostics[:16384]}, tests=[])


def bounded_process(args, *, timeout, data=None, env=None, limit=MAX_OUTPUT):
    """Drain both pipes with a strict combined cap; kill the client on cancellation."""
    process = subprocess.Popen(args, stdin=subprocess.PIPE if data is not None else subprocess.DEVNULL,
                               stdout=subprocess.PIPE, stderr=subprocess.PIPE, env=env,
                               start_new_session=True)
    selector = selectors.DefaultSelector()
    output = {"stdout": bytearray(), "stderr": bytearray()}
    for name in output:
        pipe = getattr(process, name)
        os.set_blocking(pipe.fileno(), False)
        selector.register(pipe, selectors.EVENT_READ, name)
    remaining = memoryview(data or b"")
    if data is not None:
        os.set_blocking(process.stdin.fileno(), False)
        selector.register(process.stdin, selectors.EVENT_WRITE, "stdin")
    deadline = time.monotonic() + timeout
    try:
        while selector.get_map():
            if time.monotonic() >= deadline:
                raise CheckError("timeout", "Sandbox wall-clock limit exceeded")
            for key, _ in selector.select(min(0.1, max(0, deadline - time.monotonic()))):
                if key.data == "stdin":
                    try:
                        count = os.write(key.fd, remaining[:4096]) if remaining else 0
                        remaining = remaining[count:]
                    except BrokenPipeError:
                        remaining = memoryview(b"")
                    if not remaining:
                        selector.unregister(key.fileobj)
                        key.fileobj.close()
                    continue
                chunk = os.read(key.fd, 8192)
                if not chunk:
                    selector.unregister(key.fileobj)
                    continue
                output[key.data].extend(chunk)
                if sum(map(len, output.values())) > limit:
                    raise CheckError("output_limit", "Sandbox output limit exceeded")
        process.wait(timeout=max(0.01, deadline - time.monotonic()))
        return process.returncode, bytes(output["stdout"]), bytes(output["stderr"])
    except subprocess.TimeoutExpired as error:
        raise CheckError("timeout", "Sandbox wall-clock limit exceeded") from error
    finally:
        selector.close()
        if process.poll() is None:
            try:
                os.killpg(process.pid, signal.SIGKILL)
            except ProcessLookupError:
                pass
            process.wait()
        for pipe in (process.stdin, process.stdout, process.stderr):
            if pipe is not None:
                pipe.close()


def docker_prefix(binary, daemon_socket, config):
    # Never consult DOCKER_HOST/CONTEXT or a user's credential/config directory.
    if not daemon_socket.startswith("/") or not stat.S_ISSOCK(os.stat(daemon_socket).st_mode):
        raise CheckError("sandbox_unavailable", "An explicit local Unix Docker socket is required")
    return [binary, "--config", config, "--host", "unix://" + daemon_socket]


def docker_json(prefix, arguments, env):
    code, out, _ = bounded_process(prefix + arguments, timeout=10, env=env)
    if code:
        raise CheckError("sandbox_unavailable", "Local Docker inspection failed")
    try:
        return json.loads(out)
    except (ValueError, UnicodeError) as error:
        raise CheckError("sandbox_unavailable", "Invalid local Docker inspection response") from error


def validate_daemon(info):
    options = info.get("SecurityOptions", [])
    if (info.get("OSType") != "linux" or str(info.get("CgroupVersion")) != "2"
            or not info.get("MemoryLimit") or not info.get("PidsLimit")
            or not info.get("SwapLimit")
            or not info.get("CpuCfsPeriod") or not info.get("CpuCfsQuota")
            or not any("name=seccomp" in option and "unconfined" not in option for option in options)
            or not any("name=cgroupns" in option for option in options)):
        raise CheckError("sandbox_unavailable", "Linux cgroup v2, CPU/memory/PID controls and seccomp are required")
    warnings = " ".join(info.get("Warnings") or []).lower()
    if any(word in warnings for word in ("limit", "cgroup", "seccomp")):
        raise CheckError("sandbox_unavailable", "Docker reports unavailable security/resource controls")


def run_arguments(name, image):
    return ["run", "--pull=never", "--name", name, "--network=none", "--read-only",
            "--user", "65532:65532", "--cap-drop=ALL", "--security-opt=no-new-privileges:true",
            "--cgroupns=private", "--ipc=none", "--pids-limit=32", "--cpus=1",
            "--memory=384m", "--memory-swap=384m", "--ulimit", "cpu=35:35",
            "--ulimit", "nofile=64:64", "--ulimit", "nproc=32:32",
            "--ulimit", "fsize=16777216:16777216", "--ulimit", "core=0:0",
            "--tmpfs", "/work:rw,exec,nosuid,nodev,size=67108864,mode=700,uid=65532,gid=65532",
            "--tmpfs", "/tmp:rw,noexec,nosuid,nodev,size=16777216,mode=1777",
            "--workdir=/work", "--log-driver=none", "--stop-timeout=1",
            "--entrypoint=/usr/bin/python3", "-i", image, "/opt/checker/worker.py"]


def validate_worker_report(value, submission, manifest):
    report = empty_report(submission)
    if not isinstance(value, dict) or value.get("status") not in STATUSES:
        raise CheckError("runner_error", "Invalid worker report")
    for key in ("schema", "trust", "exercise_id", "exercise_revision", "suite_id",
                "source_sha256", "attempt_id", "assistance"):
        if value.get(key) != report[key]:
            raise CheckError("runner_error", "Worker identity mismatch")
    compiler, compiled = value.get("compiler"), value.get("compile")
    if (not isinstance(compiler, dict) or compiler.get("standard") != "c++23"
            or not isinstance(compiler.get("id"), str) or len(compiler["id"]) > 80
            or not isinstance(compiler.get("version"), str) or len(compiler["version"]) > 256
            or not isinstance(compiler.get("flags"), list) or len(compiler["flags"]) > 16
            or any(not isinstance(flag, str) or len(flag) > 128 for flag in compiler["flags"])
            or not isinstance(compiled, dict) or compiled.get("status") not in {"passed", "failed", "not_run"}
            or not isinstance(compiled.get("diagnostics"), str) or len(compiled["diagnostics"]) > 16384):
        raise CheckError("runner_error", "Invalid compiler metadata")
    if compiled["status"] != "not_run" and (compiler["id"] != "gcc"
            or not compiler["version"].startswith("14.") or compiler["flags"] != FLAGS):
        raise CheckError("runner_error", "Unexpected compiler configuration")
    tests = value.get("tests")
    if not isinstance(tests, list) or len(tests) > len(manifest["cases"]):
        raise CheckError("runner_error", "Invalid test results")
    if value["status"] in {"tests_passed", "tests_failed"}:
        if compiled["status"] != "passed" or len(tests) != len(manifest["cases"]):
            raise CheckError("runner_error", "Incomplete test results")
        previous_case = previous_row = None
        for row, case in zip(tests, manifest["cases"]):
            actual = row.get("actual") if isinstance(row, dict) else None
            if (not isinstance(row, dict) or row.get("id") != case["id"] or not isinstance(actual, dict)
                    or set(actual) != {"result", "pending"}
                    or any(not isinstance(items, list) or len(items) > 1024
                           or any(type(item) is not int or not -(2**31) <= item < 2**31 for item in items)
                           for items in actual.values())):
                raise CheckError("runner_error", "Invalid actual test values")
            continuing = (case.get("sequence") is not None and previous_case is not None
                          and previous_case.get("sequence") == case["sequence"])
            expected_input = previous_row["actual"]["pending"] if continuing else case["pending"]
            if (not isinstance(row.get("input_pending"), list)
                    or any(type(item) is not int for item in row["input_pending"])
                    or row["input_pending"] != expected_input):
                raise CheckError("runner_error", "Invalid actual input sequence")
            expected_status = "passed" if actual == case["expected"] else "failed"
            if row.get("status") != expected_status:
                raise CheckError("runner_error", "Inconsistent test comparison")
            previous_case, previous_row = case, row
        expected = "tests_passed" if all(row["status"] == "passed" for row in tests) else "tests_failed"
        if value["status"] != expected:
            raise CheckError("runner_error", "Inconsistent final test result")
    elif tests:
        raise CheckError("runner_error", "Unexpected test results for infrastructure/compile failure")
    return value


def check(submission, image, daemon_socket):
    report = empty_report(submission)
    if not re.fullmatch(r"sha256:[0-9a-f]{64}", image):
        raise ValueError("--image must be an immutable local image ID sha256:<64 lowercase hex>")
    name = "cpp-recall-local-" + uuid.uuid4().hex
    binary = shutil.which("docker")
    if not binary:
        return empty_report(submission, "sandbox_unavailable", "Docker CLI is unavailable")
    env = {"PATH": os.defpath + ":/usr/local/bin", "LANG": "C.UTF-8"}
    with tempfile.TemporaryDirectory(prefix="cpp-recall-docker-config-") as config:
        prefix = None
        created = False
        try:
            prefix = docker_prefix(binary, daemon_socket, config)
            validate_daemon(docker_json(prefix, ["info", "--format", "{{json .}}"], env))
            image_info = docker_json(prefix, ["image", "inspect", image], env)
            if (not isinstance(image_info, list) or len(image_info) != 1 or image_info[0].get("Id") != image
                    or image_info[0].get("Config", {}).get("Volumes")):
                raise CheckError("sandbox_unavailable", "Reviewed local image required; image volumes are forbidden")
            manifest = json.loads(MANIFEST.read_text(encoding="utf-8"))
            created = True  # Cleanup also covers cancellation during container creation.
            code, output, _ = bounded_process(prefix + run_arguments(name, image), timeout=45,
                                              data=json.dumps(submission, ensure_ascii=False).encode(), env=env)
            state = docker_json(prefix, ["inspect", "--format", "{{json .State}}", name], env)
            if state.get("OOMKilled"):
                raise CheckError("resource_limit", "Sandbox memory limit exceeded")
            if code != 0 or state.get("ExitCode") != 0:
                raise CheckError("runner_error", "Sandbox worker failed")
            report = validate_worker_report(json.loads(output), submission, manifest)
        except CheckError as error:
            report = empty_report(submission, error.status, str(error))
        except (OSError, ValueError, TypeError, KeyError) as error:
            report = empty_report(submission, "sandbox_unavailable" if not created else "runner_error",
                                  "Local sandbox or report unavailable")
        finally:
            if prefix is not None and created:
                try:
                    cleanup, _, _ = bounded_process(prefix + ["rm", "--force", name], timeout=10, env=env)
                    if cleanup:
                        report = empty_report(submission, "runner_error", "Container cleanup failed: " + name)
                except (CheckError, OSError):
                    report = empty_report(submission, "runner_error", "Container cleanup failed: " + name)
    return report


def main():
    def cancelled(_signal, _frame):
        raise KeyboardInterrupt

    signal.signal(signal.SIGTERM, cancelled)
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("submission", type=Path)
    parser.add_argument("--report", "--output", dest="report", type=Path, required=True)
    parser.add_argument("--image", required=True)
    parser.add_argument("--daemon-socket", default="/var/run/docker.sock",
                        help="Explicit local Unix socket; remote Docker contexts are never used")
    arguments = parser.parse_args()
    try:
        with arguments.submission.open("rb") as source_file:
            raw = source_file.read(131073)
        if len(raw) > 131072:
            raise ValueError("Submission file is too large")
        submission = validate_submission(json.loads(raw.decode("utf-8")))
        report = check(submission, arguments.image, arguments.daemon_socket)
        if arguments.report.resolve() == arguments.submission.resolve():
            raise ValueError("Report must not overwrite submission")
        with arguments.report.open("x", encoding="utf-8") as destination:
            json.dump(report, destination, ensure_ascii=False, indent=2)
            destination.write("\n")
        print(report["status"])
        return 0 if report["status"] == "tests_passed" else 1
    except KeyboardInterrupt:
        print("Checker cancelled; attempt container cleanup requested", file=sys.stderr)
        return 130
    except (OSError, ValueError, UnicodeError) as error:
        print("Checker input/output error: " + str(error), file=sys.stderr)
        return 2


if __name__ == "__main__":
    sys.exit(main())
