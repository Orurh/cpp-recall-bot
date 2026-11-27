#!/usr/bin/env python3
"""Fast, read-only checks shared by local development and CI.

No CMake configuration, compilation, database or production credentials needed.
The C++ file set mirrors the existing CMake format-check target.
"""
import argparse
from pathlib import Path
import re
import shutil
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]


def run(args):
    print("+ " + " ".join(map(str, args)), flush=True)
    subprocess.run(args, cwd=ROOT, check=True)


def formatter():
    path = shutil.which("clang-format-20")
    if path is None:
        raise RuntimeError("clang-format-20 is required; install the version 20 package.")
    version = subprocess.check_output([path, "--version"], text=True)
    if not re.search(r"\bversion 20\.", version):
        raise RuntimeError("Expected clang-format major 20, got: " + version.strip())
    print(version.strip(), flush=True)
    return path


def cpp_sources():
    return sorted({
        str(p.relative_to(ROOT))
        for pattern in ("src/**/*.cc", "src/**/*.h", "tests/**/*.cc")
        for p in ROOT.glob(pattern)
        if p.is_file()
    })


def javascript_sources():
    return sorted({
        str(p.relative_to(ROOT))
        for pattern in ("docs/miniapp/**/*.js", "docs/miniapp/**/*.mjs", "tests/**/*.mjs")
        for p in ROOT.glob(pattern)
        if p.is_file()
    })


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--format-only", action="store_true",
                        help="Check only C++ formatting (no Node dependency)")
    args = parser.parse_args()
    try:
        if not args.format_only:
            run([sys.executable, "scripts/check_release_files.py"])
        sources = cpp_sources()
        if not sources:
            raise RuntimeError("No C++ sources found")
        run([formatter(), "--dry-run", "--Werror", *sources])
        if not args.format_only:
            node = shutil.which("node")
            if node is None:
                raise RuntimeError("Node.js 22 is required for JavaScript syntax checks")
            version = subprocess.check_output([node, "--version"], text=True).strip()
            if not re.match(r"^v22\.", version):
                raise RuntimeError("Expected Node.js major 22, got: " + version)
            for source in javascript_sources():
                run([node, "--check", source])
        print("Preflight: PASS (format only)" if args.format_only else "Preflight: PASS")
        return 0
    except (RuntimeError, OSError, subprocess.CalledProcessError) as error:
        print(f"Preflight: FAIL: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
