#!/usr/bin/env python3
"""Reject known local dumps, secrets and build artifacts in a release tree."""
from pathlib import PurePosixPath
import re
import subprocess
import sys


def forbidden(path):
    parts = PurePosixPath(path).parts
    return any(
        part in {'.git', '__pycache__', '.pytest_cache', 'build', 'all-info.txt', 'all_json.txt'}
        or part.startswith('build-')
        or (part.startswith('.env') and part != '.env.example')
        or re.fullmatch(r'all[-_]code.*\.txt', part, re.IGNORECASE)
        or part.endswith(('.dump', '.partial', '.pyc', '.sqlite', '.sqlite3', '.pem', '.key'))
        for part in parts
    )


def validate(paths):
    bad = [path for path in paths if forbidden(path)]
    if bad:
        # Only paths, never contents: a rejected file can contain credentials.
        raise ValueError('Release contains forbidden files: ' + ', '.join(bad))


if __name__ == '__main__':
    try:
        paths = subprocess.check_output(['git', 'ls-files', '-z']).decode().split('\0')
        validate(path for path in paths if path)
        print('Release file policy: OK (tracked files only)')
    except (ValueError, subprocess.CalledProcessError) as error:
        print(str(error), file=sys.stderr)
        sys.exit(1)
